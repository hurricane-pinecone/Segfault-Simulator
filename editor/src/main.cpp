#include "build/headlessExport.h"
#include "editorApp.h"

#include <string_view>

// SfsEditor [project-folder]
// SfsEditor --export <project-folder> [output-folder]
int main(int argc, char* argv[])
{
  if (argc >= 3 && std::string_view(argv[1]) == "--export")
    return exportHeadless(argv[2], argc >= 4 ? std::filesystem::path(argv[3])
                                             : std::filesystem::path());

  EditorApp editor(argc > 1 ? std::filesystem::path(argv[1])
                            : std::filesystem::path());
  if (!editor.init(1600, 900))
    return 1;
  editor.setup();
  editor.run();
  return 0;
}
