// Stands in for sfs-codegen: writes the registration and the "real" traits
// only when they differ, as codegen does.
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

void writeIfChanged(const char* path, const std::string& text)
{
  std::string was;
  if (FILE* in = std::fopen(path, "rb"))
  {
    char buffer[512];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, in)) > 0)
      was.append(buffer, read);
    std::fclose(in);
  }
  if (was == text)
    return;
  if (FILE* out = std::fopen(path, "wb"))
  {
    std::fputs(text.c_str(), out);
    std::fclose(out);
  }
}

} // namespace

int main(int argc, char** argv)
{
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--event-traits") == 0)
      writeIfChanged(argv[i + 1], "#pragma once\n#define REAL_TRAITS 1\n");
    else if (std::strcmp(argv[i], "--output") == 0)
      writeIfChanged(argv[i + 1], "void fixtureRegister() {}\n");
  return 0;
}
