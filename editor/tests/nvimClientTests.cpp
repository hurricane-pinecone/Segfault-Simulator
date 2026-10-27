#include "../../engine/tests/testHarness.h"

#include "build/buildEnvironment.h"
#include "nvim/nvimClient.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <thread>

// Drives a real `nvim --embed` through the client, with the engine's config.
// Skipped (passing) when Neovim isn't installed.

namespace
{
std::string rowText(const NvimScreen& screen, int row)
{
  std::string text;
  for (int c = 0; c < screen.columns(); ++c)
    text += screen.cell(row, c).text;
  return text;
}

bool screenContains(const NvimScreen& screen, const std::string& text)
{
  for (int r = 0; r < screen.rows(); ++r)
    if (rowText(screen, r).find(text) != std::string::npos)
      return true;
  return false;
}

// Poll until `done` or a few seconds pass.
bool waitFor(NvimClient& nvim, const std::function<bool()>& done)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline)
  {
    nvim.poll();
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}
} // namespace

int main()
{
  if (std::system("nvim --version > /dev/null 2>&1") != 0)
  {
    std::puts("nvim not installed; skipping");
    return 0;
  }

  const std::filesystem::path work = std::filesystem::temp_directory_path() / "sfs-nvim-client-test";
  std::filesystem::remove_all(work);
  std::filesystem::create_directories(work);

  const BuildEnvironment environment = BuildEnvironment::current();
  NvimClient nvim(environment);
  NvimClient::LaunchOptions options;
  options.mode = NvimClient::ConfigMode::Engine;
  options.engineConfig = SFS_TEST_NVIM_CONFIG;
  options.workingDirectory = work;

  TEST("Neovim should start and draw its first screen")
  {
    CHECK(nvim.start(options, 60, 12));
    CHECK(waitFor(nvim, [&] { return nvim.screen().rows() == 12; }));
    CHECK(nvim.screen().columns() == 60);
    CHECK(nvim.running());
  }

  TEST("typed text should appear, and the cursor shape follow the mode")
  {
    nvim.input("iHello <lt>world>");
    CHECK(waitFor(nvim, [&] { return screenContains(nvim.screen(), "Hello <world>"); }));
    CHECK(nvim.screen().cursorShape() == NvimScreen::CursorShape::Vertical);
    nvim.input("<Esc>");
    CHECK(waitFor(nvim, [&]
                  { return nvim.screen().cursorShape() == NvimScreen::CursorShape::Block; }));
  }

  TEST("the engine config should be loaded (absolute line numbers)")
  {
    CHECK(waitFor(nvim, [&] { return rowText(nvim.screen(), 0).find("1 Hello") != std::string::npos; }));
  }

  TEST("resizing should resize the grid")
  {
    nvim.resize(40, 8);
    CHECK(waitFor(nvim, [&] { return nvim.screen().columns() == 40 && nvim.screen().rows() == 8; }));
  }

  TEST("opening a file should show it, and Ctrl+S save it")
  {
    const std::filesystem::path file = work / "Spinner.h";
    {
      std::FILE* out = std::fopen(file.string().c_str(), "w");
      std::fputs("struct Spinner {};\n", out);
      std::fclose(out);
    }
    nvim.input("<Esc>:enew!<CR>");
    nvim.open(file);
    CHECK(waitFor(nvim, [&] { return screenContains(nvim.screen(), "struct Spinner {};"); }));
    CHECK(waitFor(nvim, [&] { return nvim.screen().title().find("Spinner.h") != std::string::npos; }));

    nvim.input("ggO// edited<Esc><C-s>");
    std::string contents;
    CHECK(waitFor(nvim,
                  [&]
                  {
                    std::FILE* in = std::fopen(file.string().c_str(), "r");
                    char line[64] = {};
                    contents = in && std::fgets(line, sizeof(line), in) ? line : "";
                    if (in)
                      std::fclose(in);
                    return contents == "// edited\n";
                  }));
  }

  TEST(":qa should end the process")
  {
    nvim.input(":qa!<CR>");
    CHECK(waitFor(nvim, [&] { return !nvim.running(); }));
  }

  TEST("stopping should save modified files and leave no swap file")
  {
    const std::filesystem::path file = work / "Unsaved.h";
    {
      std::FILE* out = std::fopen(file.string().c_str(), "w");
      std::fputs("x\n", out);
      std::fclose(out);
    }
    CHECK(nvim.start(options, 60, 12));
    nvim.open(file);
    CHECK(waitFor(nvim, [&] { return nvim.screen().title().find("Unsaved.h") != std::string::npos; }));
    nvim.input("ggOunsaved<Esc>");
    CHECK(waitFor(nvim, [&] { return screenContains(nvim.screen(), "unsaved"); }));

    // Swap files live in the engine config's own state folder.
    const char* home = std::getenv("HOME");
    const std::filesystem::path swapFolder =
        std::filesystem::path(home ? home : "") / ".local/state/sfs-editor/swap";
    const auto swapCount = [&]
    {
      int count = 0;
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator(swapFolder, error))
        count += entry.path().filename().string().find("Unsaved.h") != std::string::npos;
      return count;
    };
    CHECK(swapCount() == 1);

    nvim.stop();
    CHECK(!nvim.running());
    CHECK(swapCount() == 0);
    std::FILE* in = std::fopen(file.string().c_str(), "r");
    char line[32] = {};
    CHECK(in && std::fgets(line, sizeof(line), in) && std::string(line) == "unsaved\n");
    if (in)
      std::fclose(in);
  }

  std::filesystem::remove_all(work);
  return testing::report("nvimClientTests");
}
