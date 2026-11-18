#include "widgets/codeFont.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// stb_truetype reads the fonts' metrics before they're added. Compiled here as
// ImGui compiles its own copy: static, so the two never clash.
#if defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#if defined(__clang__)
  #pragma clang diagnostic pop
#elif defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif

namespace
{
// The size merged fonts' glyph offsets and advances are given at; ImGui scales
// them to the size drawn.
constexpr float REFERENCE_SIZE = 16.0f;

// Departure Mono is drawn on a pixel grid whose em is 11 pixels (its
// README: "set the font size to increments of 11px").
constexpr float DEPARTURE_MONO_EM_PIXELS = 11.0f;

const char* const DEPARTURE_MONO = "DepartureMono/DepartureMono-Regular.otf";
const char* const SYMBOLS_NERD_FONT =
    "SymbolsNerdFont/SymbolsNerdFontMono-Regular.ttf";

// A font file read into memory, with the metrics merging needs (in font
// units). Its data is ImGui-allocated, for the atlas to take over.
struct FontFile
{
  std::filesystem::path path;
  void* data = nullptr;
  int size = 0;
  float height = 0.0f; // ascent - descent: what ImGui sizes a font by
  float centre = 0.0f; // midway between ascent and descent, above the baseline
  float cell = 0.0f;   // one column: the advance of 'M', or the widest glyph
  float em = 0.0f;     // units per em
};

uint16_t readUint16(const unsigned char* bytes)
{
  return static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
}

// The file's font and its metrics; nullopt (logged) if it can't be read.
std::optional<FontFile> readFontFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);

  if (!in)
  {
    LOG_WARN("Code panel font not readable: " + path.string());
    return std::nullopt;
  }

  FontFile file;
  file.path = path;
  file.size = static_cast<int>(in.tellg());
  file.data = IM_ALLOC(file.size);
  in.seekg(0);
  in.read(static_cast<char*>(file.data), file.size);

  const auto* bytes = static_cast<const unsigned char*>(file.data);
  stbtt_fontinfo info;

  if (!in ||
      !stbtt_InitFont(&info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0)))
  {
    LOG_WARN("Code panel font not a font: " + path.string());
    IM_FREE(file.data);
    return std::nullopt;
  }

  int ascent = 0, descent = 0, lineGap = 0;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
  file.height = static_cast<float>(ascent - descent);
  file.centre = 0.5f * static_cast<float>(ascent + descent);
  file.em = static_cast<float>(readUint16(info.data + info.head + 18));

  if (stbtt_FindGlyphIndex(&info, 'M') != 0)
  {
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&info, 'M', &advance, &bearing);
    file.cell = static_cast<float>(advance);
  }
  else // a symbols font: every glyph is one cell wide (hhea.advanceWidthMax)
    file.cell = static_cast<float>(readUint16(info.data + info.hhea + 10));

  if (file.height <= 0.0f || file.cell <= 0.0f || file.em <= 0.0f)
  {
    LOG_WARN("Code panel font has no usable metrics: " + path.string());
    IM_FREE(file.data);
    return std::nullopt;
  }

  return file;
}

// Add `file` as the code font; the atlas owns its data from here on.
ImFont* addFont(FontFile& file, bool pixelFont)
{
  ImFontConfig config;
  // Only the reference for merged glyphs' offsets: ImGui 1.92 draws a font at
  // the size pushed.
  config.SizePixels = REFERENCE_SIZE;
  config.OversampleH = 1; // crisp at the pixel sizes code is read at

  if (pixelFont)
  {
    config.OversampleV = 1;
    config.PixelSnapH = true;
  }

  ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
      file.data, file.size, 0.0f, &config);
  file.data = nullptr;
  return font;
}

// Merge `file` into `font` (made from `main`) for the glyphs `main` lacks:
// scaled so one of its cells is one of `main`'s, centred on `main`'s line.
// The merged font never changes the line height (only the main font's
// ascent and descent count) or the cell width. Whether it was merged.
bool mergeFont(ImFont* font, const FontFile& main, FontFile& file)
{
  const float mainCell = main.cell / main.height; // per pixel of font size
  const float scale = mainCell / (file.cell / file.height);

  ImFontConfig config;
  config.MergeMode = true;
  config.DstFont = font;
  config.SizePixels = REFERENCE_SIZE;
  config.OversampleH = 1;
  config.ExtraSizeScale = scale;
  config.GlyphMinAdvanceX = REFERENCE_SIZE * mainCell;
  config.GlyphMaxAdvanceX = REFERENCE_SIZE * mainCell;
  config.GlyphOffset.y = REFERENCE_SIZE * (file.centre / file.height * scale -
                                           main.centre / main.height);

  const bool merged = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                          file.data, file.size, 0.0f, &config) != nullptr;
  file.data = nullptr;

  if (merged)
    LOG_INFO("Code panel font, merged in: " + file.path.string());

  return merged;
}

bool isNerdFont(const std::filesystem::path& path)
{
  return path.filename().string().find("NerdFont") != std::string::npos;
}

// The first installed Nerd Font Mono (icons in statuslines and file trees) in
// `folder`, if any.
std::optional<std::filesystem::path>
findNerdFont(const std::filesystem::path& folder)
{
  std::error_code error;

  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(folder, error))
  {
    const std::string name = entry.path().filename().string();

    if (name.find("NerdFontMono-Regular.") != std::string::npos)
      return entry.path();
  }

  return std::nullopt;
}

// An installed monospace font: a Nerd Font Mono, else the platform's standard
// coding font. Empty if none is found.
std::filesystem::path installedCodeFont()
{
  std::vector<std::filesystem::path> nerdFolders, fallbacks;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA"))
    nerdFolders.push_back(std::filesystem::path(local) / "Microsoft" /
                          "Windows" / "Fonts");

  nerdFolders.push_back("C:\\Windows\\Fonts");
  fallbacks = {"C:\\Windows\\Fonts\\CascadiaMono.ttf",
               "C:\\Windows\\Fonts\\consola.ttf"};
#else
  if (const char* home = std::getenv("HOME"))
    nerdFolders.push_back(std::filesystem::path(home) / ".local" / "share" /
                          "fonts");

  nerdFolders.push_back("/usr/share/fonts");
  fallbacks = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
               "/usr/share/fonts/TTF/DejaVuSansMono.ttf"};
#endif

  for (const auto& folder : nerdFolders)
    if (const auto font = findNerdFont(folder))
      return *font;

  std::error_code error;

  for (const auto& font : fallbacks)
    if (std::filesystem::exists(font, error))
      return font;

  return {};
}
} // namespace

CodeFont loadCodeFont(const std::filesystem::path& fontsFolder,
                      const std::filesystem::path& uiFontFile)
{
  // The candidates in order of preference; the first that loads is the code
  // font.
  std::vector<std::filesystem::path> candidates;

  if (const char* custom = std::getenv("SFS_EDITOR_CODE_FONT");
      custom && *custom)
    candidates.push_back(custom);

  const std::filesystem::path departureMono = fontsFolder / DEPARTURE_MONO;
  const std::filesystem::path installed = installedCodeFont();
  std::error_code error;

  if (std::filesystem::exists(departureMono, error))
    candidates.push_back(departureMono);
  else
    LOG_WARN("Code panel: the shipped font is missing: " +
             departureMono.string());

  if (!installed.empty())
    candidates.push_back(installed);

  if (!uiFontFile.empty())
    candidates.push_back(uiFontFile);

  // The first font is the UI's: keep ImGui's default there (it is otherwise
  // only added when the atlas is still empty at the first frame).
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;

  if (atlas->Fonts.empty())
    atlas->AddFontDefault();

  for (const auto& path : candidates)
  {
    std::optional<FontFile> main = readFontFile(path);

    if (!main)
      continue;

    const bool pixelFont =
        path.filename().string().starts_with("DepartureMono");
    CodeFont result;
    result.font = addFont(*main, pixelFont);

    if (!result.font)
    {
      LOG_WARN("Code panel font couldn't be loaded: " + path.string());
      continue;
    }

    LOG_INFO("Code panel font: " + path.string());
    result.icons = isNerdFont(path);

    if (pixelFont)
      result.pixelSize = main->height / main->em * DEPARTURE_MONO_EM_PIXELS;

    if (std::optional<FontFile> symbols =
            readFontFile(fontsFolder / SYMBOLS_NERD_FONT))
      result.icons |= mergeFont(result.font, *main, *symbols);

    // The pixel font covers Latin, box drawing and the common symbols; an
    // installed coding font fills in the rest.
    if (pixelFont && !installed.empty())
      if (std::optional<FontFile> fallback = readFontFile(installed))
        result.icons |=
            mergeFont(result.font, *main, *fallback) && isNerdFont(installed);

    return result;
  }

  return {};
}

float codeFontSize(const CodeFont& font, float requested, float scale)
{
  if (font.pixelSize <= 0.0f || scale <= 0.0f)
    return requested;

  const float multiple =
      std::max(1.0f, std::round(requested * scale / font.pixelSize));
  return multiple * font.pixelSize / scale;
}
