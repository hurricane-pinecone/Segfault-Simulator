#include "../../engine/tests/testHarness.h"
#include "build/buildEnvironment.h"
#include "nvim/nvimClient.h"
#include "nvim/nvimScreen.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// Checks the command the client starts Neovim with, then drives a real
// `nvim --embed` through it, with the engine's config: the editor's own Neovim
// and tools when this build ships them (SFS_EDITOR_BUNDLE_TOOLS), otherwise
// the ones on PATH. The live checks are skipped (passing) when there's no
// Neovim, and on Windows.

namespace
{
// Windows names executables with ".exe" and separates PATH's folders with ';'.
constexpr bool WINDOWS = std::filesystem::path::preferred_separator == '\\';
constexpr const char* EXECUTABLE_SUFFIX = WINDOWS ? ".exe" : "";
constexpr char PATH_SEPARATOR = WINDOWS ? ';' : ':';

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

// The value a launch command sets for `name`; empty if it sets none.
std::string variable(const NvimClient::LaunchCommand& command,
                     const std::string& name)
{
  for (const auto& [key, value] : command.environment)
    if (key == name)
      return value;

  return {};
}

bool contains(const std::vector<std::string>& args, const std::string& arg)
{
  return std::find(args.begin(), args.end(), arg) != args.end();
}

void touch(const std::filesystem::path& file)
{
  std::filesystem::create_directories(file.parent_path());
  std::FILE* out = std::fopen(file.string().c_str(), "w");

  if (out)
    std::fclose(out);
}

// Polls until `done`, however long it takes; false if nvim stops first.
bool waitFor(NvimClient& nvim, const std::function<bool()>& done)
{
  for (;;)
  {
    nvim.poll();

    if (done())
      return true;

    if (!nvim.running())
      return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
} // namespace

int main()
{
  const std::filesystem::path work =
      std::filesystem::temp_directory_path() / "sfs-nvim-client-test";
  std::filesystem::remove_all(work);
  std::filesystem::create_directories(work);

  const BuildEnvironment environment = BuildEnvironment::current();
  NvimClient nvim(environment);
  NvimClient::LaunchOptions options;
  options.mode = NvimClient::ConfigMode::Engine;
  options.engineConfig = SFS_TEST_NVIM_CONFIG;
  options.workingDirectory = work;

  // Tests build only where the editor runs its tools itself (not through WSL).
  TEST("with shipped tools, the editor's own Neovim should start, with its "
       "runtime and the tools first on PATH")
  {
    const std::filesystem::path tools = work / "shipped-tools";
    const std::filesystem::path shippedNvim =
        tools / "nvim" / "bin" / (std::string("nvim") + EXECUTABLE_SUFFIX);
    touch(shippedNvim);
    std::filesystem::create_directories(tools / "nvim/share/nvim/runtime");
    touch(tools / "ripgrep" / (std::string("rg") + EXECUTABLE_SUFFIX));
    touch(tools / "clangd" / "bin" /
          (std::string("clangd") + EXECUTABLE_SUFFIX));

    NvimClient::LaunchOptions shipped = options;
    shipped.tools = tools;
    const NvimClient::LaunchCommand command = nvim.launchCommand(shipped);

    CHECK(command.bundled);
    CHECK(!command.args.empty() && command.args[0] == shippedNvim.string());
    CHECK(contains(command.args, "--embed"));
    CHECK(contains(command.args, (options.engineConfig / "init.lua").string()));
    CHECK(!contains(command.args, "env"));

    const auto folder = [&](const char* name)
    { return (tools / name).make_preferred().string(); };
    const std::string first = folder("nvim/bin") + PATH_SEPARATOR +
                              folder("ripgrep") + PATH_SEPARATOR +
                              folder("clangd/bin");
    const std::string path = variable(command, "PATH");
    CHECK(path.starts_with(first));
    const char* inherited = std::getenv("PATH");

    if (inherited && *inherited)
      CHECK(path == first + PATH_SEPARATOR + inherited);

    CHECK(variable(command, "VIMRUNTIME") ==
          (tools / "nvim/share/nvim/runtime").make_preferred().string());
    CHECK(variable(command, "NVIM_APPNAME") == "sfs-editor");
    CHECK(variable(command, "SFS_LSP") == environment.lsp());
  }

  TEST("without shipped tools, Neovim and its tools should come from PATH")
  {
    NvimClient::LaunchOptions missing = options;
    missing.tools = work / "no-tools";
    const NvimClient::LaunchCommand command = nvim.launchCommand(missing);

    CHECK(!command.bundled);
    CHECK(!command.args.empty() && command.args[0] == "nvim");
    CHECK(variable(command, "PATH").empty());
    CHECK(variable(command, "VIMRUNTIME").empty());
    CHECK(variable(command, "NVIM_APPNAME") == "sfs-editor");

    missing.tools.clear();
    CHECK(nvim.launchCommand(missing).args[0] == "nvim");
  }

#if defined(_WIN32)
  std::puts("the live Neovim checks run on Linux and macOS; skipping them");
  std::filesystem::remove_all(work);
  return testing::report("nvimClientTests");
#else
  options.tools = SFS_TEST_EDITOR_TOOLS;
  const NvimClient::LaunchCommand launch = nvim.launchCommand(options);
  const std::string nvimProgram = launch.args.empty() ? "" : launch.args[0];

  if (testing::runCommand("'" + nvimProgram + "' --version > /dev/null 2>&1") !=
      0)
  {
    std::puts("nvim not installed; skipping the live checks");
    std::filesystem::remove_all(work);
    return testing::report("nvimClientTests");
  }

  std::puts(launch.bundled ? "driving the editor's own Neovim"
                           : "driving Neovim from PATH");

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
    CHECK(waitFor(
        nvim, [&] { return screenContains(nvim.screen(), "Hello <world>"); }));
    CHECK(nvim.screen().cursorShape() == NvimScreen::CursorShape::Vertical);
    nvim.input("<Esc>");
    CHECK(waitFor(nvim,
                  [&] {
                    return nvim.screen().cursorShape() ==
                           NvimScreen::CursorShape::Block;
                  }));
  }

  TEST("the engine config should be loaded (absolute line numbers)")
  {
    CHECK(waitFor(nvim,
                  [&] {
                    return rowText(nvim.screen(), 0).find("1 Hello") !=
                           std::string::npos;
                  }));
  }

  TEST("resizing should resize the grid")
  {
    nvim.resize(40, 8);
    CHECK(waitFor(nvim,
                  [&] {
                    return nvim.screen().columns() == 40 &&
                           nvim.screen().rows() == 8;
                  }));
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
    CHECK(waitFor(
        nvim,
        [&] { return screenContains(nvim.screen(), "struct Spinner {};"); }));
    CHECK(waitFor(nvim,
                  [&] {
                    return nvim.screen().title().find("Spinner.h") !=
                           std::string::npos;
                  }));

    nvim.input("ggO// edited<Esc><C-s>");
    std::string contents;
    CHECK(waitFor(nvim,
                  [&]
                  {
                    std::FILE* in = std::fopen(file.string().c_str(), "r");
                    char line[64] = {};
                    contents =
                        in && std::fgets(line, sizeof(line), in) ? line : "";

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
    CHECK(waitFor(nvim,
                  [&] {
                    return nvim.screen().title().find("Unsaved.h") !=
                           std::string::npos;
                  }));
    nvim.input("ggOunsaved<Esc>");
    CHECK(waitFor(
        nvim, [&] { return screenContains(nvim.screen(), "unsaved"); }));

    // Swap files live in the engine config's own state folder.
    const char* home = std::getenv("HOME");
    const std::filesystem::path swapFolder =
        std::filesystem::path(home ? home : "") /
        ".local/state/sfs-editor/swap";
    const auto swapCount = [&]
    {
      int count = 0;
      std::error_code error;

      for (const auto& entry :
           std::filesystem::directory_iterator(swapFolder, error))
        count += entry.path().filename().string().find("Unsaved.h") !=
                 std::string::npos;

      return count;
    };
    CHECK(swapCount() == 1);

    nvim.stop();
    CHECK(!nvim.running());
    CHECK(swapCount() == 0);
    std::FILE* in = std::fopen(file.string().c_str(), "r");
    char line[32] = {};
    CHECK(in && std::fgets(line, sizeof(line), in) &&
          std::string(line) == "unsaved\n");

    if (in)
      std::fclose(in);
  }

  TEST("Neovim should see SFS_LSP, and the engine config start sfs-lsp only in "
       "a project or the SFS tree")
  {
    const auto write = [](const std::filesystem::path& path, const char* text)
    {
      std::filesystem::create_directories(path.parent_path());
      std::FILE* out = std::fopen(path.string().c_str(), "w");
      std::fputs(text, out);
      std::fclose(out);
    };
    write(work / "Game/project.sfsproj", "{}\n");
    write(work / "Game/scripts/Enemy.h", "struct Enemy { int a; };\n");
    write(work / "Engine/.sfs-engine",
          "# Marks the SFS source tree as a root for sfs-lsp.\n");
    write(work / "Engine/include/thing.h", "struct Thing { int a; };\n");
    write(work / "Other/loose.h", "struct Loose { int a; };\n");

    NvimClient lsp(environment);
    CHECK(lsp.start(options, 100, 12));
    // Repeats a command until its output shows `expected` (attaching is
    // asynchronous).
    const auto shows =
        [&](const std::string& command, const std::string& expected)
    {
      while (lsp.running())
      {
        lsp.input(command);

        for (int i = 0; i < 10; ++i)
        {
          lsp.poll();

          if (screenContains(lsp.screen(), expected))
            return true;

          std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
      }

      return false;
    };
    CHECK(shows(":echo 'lsp=' . ($SFS_LSP ==# '" + environment.lsp() + "')<CR>",
                "lsp=1"));

    if (std::system(("test -x '" + environment.lsp() + "'").c_str()) != 0)
      std::puts("sfs-lsp isn't built; skipping the attach checks");
    else
    {
      const std::string count =
          ":lua print('sfs_lsp=' .. #vim.lsp.get_clients({ bufnr = 0, name = "
          "'sfs_lsp' }))<CR>";
      lsp.open(work / "Game/scripts/Enemy.h");
      CHECK(shows(count, "sfs_lsp=1"));
      lsp.open(work / "Engine/include/thing.h");
      CHECK(shows(count, "sfs_lsp=1"));
      lsp.open(work / "Other/loose.h");
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      CHECK(shows(count, "sfs_lsp=0"));
    }

    lsp.stop();
  }

  if (launch.bundled)
  {
    TEST("the editor's own Neovim should find its runtime and the shipped "
         "ripgrep and clangd")
    {
      NvimClient tools(environment);
      CHECK(tools.start(options, 200, 12));
      const std::string folder =
          std::filesystem::path(SFS_TEST_EDITOR_TOOLS).string();
      // Each check echoes a marker of its own, so an earlier one's answer
      // still on screen can't pass it.
      int checks = 0;
      const auto shows = [&](const std::string& expression)
      {
        const std::string marker = "check" + std::to_string(++checks) + "=";
        const std::string command =
            ":echo '" + marker + "' . (" + expression + ")<CR>";

        for (int attempt = 0; attempt < 50 && tools.running(); ++attempt)
        {
          tools.input(command);

          for (int i = 0; i < 10; ++i)
          {
            tools.poll();

            if (screenContains(tools.screen(), marker + "1"))
              return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        }

        return false;
      };
      const std::string inTools = " =~# '^" + folder + "/'";
      CHECK(shows("v:progpath" + inTools));
      CHECK(shows("$VIMRUNTIME" + inTools));
      CHECK(shows("exepath('rg')" + inTools));
      CHECK(shows("exepath('clangd')" + inTools));
      CHECK(shows("exists(':GrugFar') == 2"));
      tools.stop();
    }
  }

  std::filesystem::remove_all(work);
  return testing::report("nvimClientTests");
#endif
}
