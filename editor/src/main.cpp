#include "editorApp.h"

// SfsEditor [project-folder]
int main(int argc, char* argv[])
{
  EditorApp editor(argc > 1 ? std::filesystem::path(argv[1])
                            : std::filesystem::path());
  if (!editor.init(1600, 900))
    return 1;
  editor.setup();
  editor.run();
  return 0;
}
