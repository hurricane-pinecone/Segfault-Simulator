#include "build/headlessExport.h"
#include "editorApp.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/cpuFeatures.h"

#include <SDL_messagebox.h>

#include <string>
#include <string_view>

// SfsEditor [project-folder]
// SfsEditor --export [--development] <project-folder> [output-folder]
// SfsEditor --export-web <project-folder> [output-folder]
int main(int argc, char* argv[])
{
  if (argc >= 3 && std::string_view(argv[1]) == "--export")
  {
    // --development: a development build (its development keys work).
    const bool development = std::string_view(argv[2]) == "--development";
    const int first = development ? 3 : 2;
    if (argc <= first)
    {
      LOG_ERROR("Usage: SfsEditor --export [--development] <project-folder> [output-folder]");
      return 1;
    }
    return exportHeadless(argv[first],
                          argc > first + 1 ? std::filesystem::path(argv[first + 1]) : std::filesystem::path(),
                          development);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "--export-web")
    return exportWebHeadless(argv[2], argc >= 4 ? std::filesystem::path(argv[3])
                                                : std::filesystem::path());

  // The engine's physics is built for AVX2 (see missingCpuFeatures): say so,
  // rather than crash at the first physics step.
  if (const std::string missing = sfs::missingCpuFeatures(); !missing.empty())
  {
    const std::string message = "SfsEditor needs a processor with " + missing +
                                " support, which this computer's doesn't have (or its operating system doesn't enable).";
    LOG_ERROR(message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SfsEditor", message.c_str(), nullptr);
    return 1;
  }

  EditorApp editor(argc > 1 ? std::filesystem::path(argv[1])
                            : std::filesystem::path());
  if (!editor.init(1600, 900))
    return 1;
  editor.setup();
  editor.run();
  return 0;
}
