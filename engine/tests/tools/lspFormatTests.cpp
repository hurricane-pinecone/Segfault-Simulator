#include "../testHarness.h"
#include "lspTestSupport.h"

#include "format.h"
#include "formatBlankLines.h"
#include "formatOrder.h"
#include "runProcess.h"
#include "server.h"

#include <iostream>
#include <random>
#include <sstream>

using namespace sfs::lsp;

namespace
{

// The fixtures: inputs and what the house passes make of them, as .txt so
// clang-format leaves them alone.
const std::filesystem::path kData =
    std::filesystem::path(SFS_LSP_TEST_SOURCE).parent_path() / "format";

std::string readText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string fixture(const std::string& name) { return readText(kData / name); }

// A folder of its own for one run of these tests.
std::filesystem::path scratch()
{
  static const std::filesystem::path folder = []
  {
    std::random_device random;
    const std::filesystem::path made =
        std::filesystem::temp_directory_path() /
        ("sfs-lsp-format-" + std::to_string(random()));
    std::filesystem::create_directories(made);
    return made;
  }();
  return folder;
}

// A stand-in for clang-format that changes nothing, so the tests see the
// house passes alone. Empty where it can't be made.
std::string identity()
{
#if defined(_WIN32)
  return {};
#else
  static const std::string path = []
  {
    const std::filesystem::path script = scratch() / "identity-format";
    lspTest::writeFile(script, "#!/bin/sh\nexec cat\n");
    std::filesystem::permissions(script,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
    return script.string();
  }();
  return path;
#endif
}

// A file's text formatted as `path` (with the house passes, under a folder
// with a .sfs-format) by the stand-in clang-format.
FormatResult formatted(const std::filesystem::path& path,
                       std::string_view text,
                       uint32_t firstLine = 0,
                       uint32_t lastLine = 0)
{
  FormatRequest request;
  request.text = text;
  request.path = path;
  request.clangFormat = identity();
  request.firstLine = firstLine;
  request.lastLine = lastLine;
  return formatSource(request, lspTest::parser());
}

// The house passes' tree for `text`.
std::unique_ptr<TSTree, decltype(&ts_tree_delete)> treeOf(std::string_view text)
{
  return {ts_parser_parse_string(lspTest::parser(),
                                 nullptr,
                                 text.data(),
                                 static_cast<uint32_t>(text.size())),
          &ts_tree_delete};
}

std::string classOrder(std::string_view text,
                       std::vector<std::string>& notes,
                       bool sortFields = false)
{
  return orderClassBodies(text, treeOf(text).get(), sortFields, notes);
}

std::string definitionOrder(std::string_view text,
                            const std::filesystem::path& path,
                            std::vector<std::string>& notes)
{
  const FileReader read = [](const std::filesystem::path& file)
  { return readText(file); };
  return orderDefinitions(
      text, treeOf(text).get(), path, lspTest::parser(), read, notes);
}

bool mentions(const std::vector<std::string>& notes, std::string_view text)
{
  for (const std::string& note : notes)
    if (note.find(text) != std::string::npos)
      return true;

  return false;
}

// Prints a mismatch so the fixture can be checked by eye.
void showIfDifferent(const std::string& got, const std::string& expected)
{
  if (got != expected)
    std::cerr << "--- got:\n" << got << "--- expected:\n" << expected;
}

} // namespace

int main()
{
  const std::filesystem::path house = scratch() / "house";
  lspTest::writeFile(house / ".sfs-format",
                     "# The house passes, for these tests.\n"
                     "sort-fields: false\n"
                     "exclude: vendor\n");
  lspTest::writeFile(house / "playerSystem.h", fixture("playerSystem.h.txt"));

  TEST("a header's classes should take the house order")
  {
    std::vector<std::string> notes;
    const std::string got = classOrder(fixture("playerSystem.h.txt"), notes);
    const std::string expected = fixture("playerSystem.h.expected.txt");
    CHECK(got == expected);
    showIfDifferent(got, expected);
    CHECK(notes.empty());
  }

  TEST("a source's definitions should take the house order")
  {
    std::vector<std::string> notes;
    const std::string ordered = definitionOrder(
        fixture("playerSystem.cpp.txt"), house / "playerSystem.cpp", notes);
    const std::string got =
        addControlBlankLines(ordered, treeOf(ordered).get());
    const std::string expected = fixture("playerSystem.cpp.expected.txt");
    CHECK(got == expected);
    showIfDifferent(got, expected);
    CHECK(notes.empty());
  }

  TEST("control blocks should get blank lines above and below")
  {
    const std::string text = fixture("blankLines.cpp.txt");
    const std::string got = addControlBlankLines(text, treeOf(text).get());
    const std::string expected = fixture("blankLines.cpp.expected.txt");
    CHECK(got == expected);
    showIfDifferent(got, expected);
  }

  TEST("formatting should be idempotent and keep every line")
  {
    if (identity().empty())
      break;

    for (const auto& [name, input] :
         {std::pair<std::string, std::string>{
              "playerSystem.h", "playerSystem.h.txt"},
          {"playerSystem.cpp", "playerSystem.cpp.txt"},
          {"blankLines.cpp", "blankLines.cpp.txt"}})
    {
      const std::string text = fixture(input);
      const FormatResult once = formatted(house / name, text);
      const FormatResult twice = formatted(house / name, once.text);
      CHECK(once.text != text);
      CHECK(twice.text == once.text);
      CHECK(sameLines(text, once.text));
      CHECK(!once.refused);
      CHECK(!once.noClangFormat);
    }
  }

  TEST("comments, docstrings and attributes should move with their "
       "declarations")
  {
    const std::string got = fixture("playerSystem.h.expected.txt");
    CHECK(got.find("  /** Makes one. */\n  explicit PlayerSystem(") !=
          std::string::npos);
    CHECK(got.find("  [[sfs::hide]]\n  float m_speed") != std::string::npos);
    CHECK(got.find("  int m_count = 0; // how many there are\n") !=
          std::string::npos);
    CHECK(got.find("  // Scratch, reused each frame.\n  std::vector<int> "
                   "m_scratch;") != std::string::npos);
  }

  TEST("overloads and equal names should keep their order")
  {
    std::vector<std::string> notes;
    const std::string got = classOrder("struct Door\n"
                                       "{\n"
                                       "  void open(float speed);\n"
                                       "  void close();\n"
                                       "  void open();\n"
                                       "  void Open(int);\n"
                                       "};\n",
                                       notes);
    CHECK(got == "struct Door\n"
                 "{\n"
                 "  void close();\n"
                 "  void open(float speed);\n"
                 "  void open();\n"
                 "  void Open(int);\n"
                 "};\n");
  }

  TEST("a struct without labels should gain one only where a section moves")
  {
    std::vector<std::string> notes;
    const std::string got = classOrder("class Enemy\n"
                                       "{\n"
                                       "  int m_health = 0;\n"
                                       "  void heal();\n"
                                       "\n"
                                       "public:\n"
                                       "  void hit();\n"
                                       "};\n",
                                       notes);
    CHECK(got == "class Enemy\n"
                 "{\n"
                 "public:\n"
                 "  void hit();\n"
                 "\n"
                 "private:\n"
                 "  void heal();\n"
                 "\n"
                 "  int m_health = 0;\n"
                 "};\n");
    CHECK(notes.empty());
  }

  TEST("fields should keep their order unless sort-fields is on")
  {
    const std::string text = "struct Color\n"
                             "{\n"
                             "  float r = 0.0f;\n"
                             "  float g = 0.0f;\n"
                             "  float b = 0.0f;\n"
                             "  float a = 1.0f;\n"
                             "};\n";
    std::vector<std::string> notes;
    CHECK(classOrder(text, notes) == text);
    CHECK(classOrder(text, notes, true) == "struct Color\n"
                                           "{\n"
                                           "  float a = 1.0f;\n"
                                           "  float b = 0.0f;\n"
                                           "  float g = 0.0f;\n"
                                           "  float r = 0.0f;\n"
                                           "};\n");
    // Merging the sections would put y before x.
    const std::string split = "class Point\n"
                              "{\n"
                              "public:\n"
                              "  float x;\n"
                              "\n"
                              "private:\n"
                              "  float y;\n"
                              "\n"
                              "public:\n"
                              "  float z;\n"
                              "};\n";
    notes.clear();
    CHECK(classOrder(split, notes) == split);
    CHECK(mentions(notes, "class Point keeps its order"));
    CHECK(mentions(notes, "order of its fields"));
    notes.clear();
    CHECK(classOrder(split, notes, true) != split);
  }

  TEST("a declaration should never move above a name it uses")
  {
    const std::string text = "class Grid\n"
                             "{\n"
                             "private:\n"
                             "  static constexpr int SIZE = 4;\n"
                             "\n"
                             "public:\n"
                             "  std::array<int, SIZE> cells() const;\n"
                             "};\n";
    std::vector<std::string> notes;
    CHECK(classOrder(text, notes) == text);
    CHECK(mentions(notes, "uses SIZE, which would come after it"));
    // A type used in a signature is in the head, before the methods.
    const std::string types = "struct Spawner\n"
                              "{\n"
                              "  void spawn(Wave wave);\n"
                              "  struct Wave\n"
                              "  {\n"
                              "    int size;\n"
                              "  };\n"
                              "};\n";
    notes.clear();
    CHECK(classOrder(types, notes) == "struct Spawner\n"
                                      "{\n"
                                      "  struct Wave\n"
                                      "  {\n"
                                      "    int size;\n"
                                      "  };\n"
                                      "\n"
                                      "  void spawn(Wave wave);\n"
                                      "};\n");
    CHECK(notes.empty());
  }

  TEST("a class with a macro or a preprocessor line should keep its order")
  {
    for (const std::string text : {"class Pickup\n"
                                   "{\n"
                                   "  int m_value;\n"
                                   "  void take();\n"
                                   "#if defined(DEBUG)\n"
                                   "  void dump();\n"
                                   "#endif\n"
                                   "};\n",
                                   "class Pickup\n"
                                   "{\n"
                                   "  REFLECT(Pickup)\n"
                                   "  void take();\n"
                                   "  void drop();\n"
                                   "};\n",
                                   "class Pickup\n"
                                   "{\n"
                                   "  void take(); void drop();\n"
                                   "  void give();\n"
                                   "};\n",
                                   "class Pickup\n"
                                   "{\n"
                                   "public: // the API\n"
                                   "  void take();\n"
                                   "  void drop();\n"
                                   "};\n"})
    {
      std::vector<std::string> notes;
      CHECK(classOrder(text, notes) == text);
      CHECK(mentions(notes, "class Pickup keeps its order"));
    }
  }

  TEST("a source with an #if region should keep its order")
  {
    const std::string text = "#include \"playerSystem.h\"\n"
                             "\n"
                             "void PlayerSystem::update(double deltaTime) {}\n"
                             "\n"
                             "#if defined(DEBUG)\n"
                             "void PlayerSystem::add(int id) {}\n"
                             "#endif\n"
                             "\n"
                             "PlayerSystem::PlayerSystem(float speed) {}\n";
    std::vector<std::string> notes;
    CHECK(definitionOrder(text, house / "playerSystem.cpp", notes) == text);
    CHECK(mentions(notes, "#if or #ifdef region"));
  }

  TEST("several classes in one source should follow their header's order")
  {
    lspTest::writeFile(house / "pair.h",
                       "struct Wall\n"
                       "{\n"
                       "  void build();\n"
                       "};\n"
                       "struct Door\n"
                       "{\n"
                       "  void open();\n"
                       "  void close();\n"
                       "};\n");
    const std::string text = "#include \"pair.h\"\n"
                             "\n"
                             "void Door::open() {}\n"
                             "\n"
                             "void Wall::build() {}\n"
                             "\n"
                             "void Door::close() {}\n";
    std::vector<std::string> notes;
    CHECK(definitionOrder(text, house / "pair.cpp", notes) ==
          "#include \"pair.h\"\n"
          "\n"
          "void Wall::build() {}\n"
          "\n"
          "void Door::close() {}\n"
          "\n"
          "void Door::open() {}\n");
    CHECK(notes.empty());
  }

  TEST("what no header declares should be worked out or left alone")
  {
    // A constructor in the file makes Spawner a class.
    const std::string guessed = "void Spawner::spawn() {}\n"
                                "\n"
                                "Spawner::Spawner() {}\n";
    std::vector<std::string> notes;
    CHECK(definitionOrder(guessed, house / "nothing.cpp", notes) ==
          "Spawner::Spawner() {}\n"
          "\n"
          "void Spawner::spawn() {}\n");
    // A namespace a header declares holds free functions, which keep their
    // order.
    lspTest::writeFile(house / "maths.h",
                       "namespace Maths\n"
                       "{\n"
                       "float lerp(float a, float b, float t);\n"
                       "float clamp(float x);\n"
                       "} // namespace Maths\n");
    const std::string free = "#include \"maths.h\"\n"
                             "\n"
                             "float Maths::lerp(float a, float b, float t) "
                             "{ return a; }\n"
                             "\n"
                             "float Maths::clamp(float x) { return x; }\n";
    notes.clear();
    CHECK(definitionOrder(free, house / "maths.cpp", notes) == free);
    CHECK(notes.empty());
    // Unknown, and nothing tells: the file keeps its order.
    const std::string unknown = "void Mystery::b() {}\n"
                                "\n"
                                "void Mystery::a() {}\n";
    notes.clear();
    CHECK(definitionOrder(unknown, house / "nothing.cpp", notes) == unknown);
    CHECK(mentions(notes, "Mystery is a class or a namespace"));
  }

  TEST("a definition another moved above needs first should keep the order")
  {
    const std::string text = "auto Enemy::size() { return 2; }\n"
                             "\n"
                             "Enemy::Enemy() { m_size = size(); }\n";
    std::vector<std::string> notes;
    CHECK(definitionOrder(text, house / "enemy.cpp", notes) == text);
    CHECK(mentions(notes, "uses size"));
  }

  TEST("a helper should stay put when moving it would change a call")
  {
    // helper(double) would move above b(), whose helper(2) meant another
    // overload.
    const std::string text = "void Spawner::b() { helper(2); }\n"
                             "\n"
                             "static void helper(double x) {}\n"
                             "\n"
                             "void Spawner::a() { helper(2.0); }\n"
                             "\n"
                             "Spawner::Spawner() {}\n";
    std::vector<std::string> notes;
    CHECK(definitionOrder(text, house / "spawner.cpp", notes) == text);
    CHECK(mentions(notes, "would see the helper declared after it"));
  }

  TEST("a divider comment should split a scope into parts that keep apart")
  {
    const std::string text = "// --- Doors ---\n"
                             "\n"
                             "void Door::open() {}\n"
                             "\n"
                             "Door::Door() {}\n"
                             "\n"
                             "// --- Walls ---\n"
                             "\n"
                             "void Wall::build() {}\n"
                             "\n"
                             "Wall::Wall() {}\n";
    std::vector<std::string> notes;
    CHECK(definitionOrder(text, house / "parts.cpp", notes) ==
          "// --- Doors ---\n"
          "\n"
          "Door::Door() {}\n"
          "\n"
          "void Door::open() {}\n"
          "\n"
          "// --- Walls ---\n"
          "\n"
          "Wall::Wall() {}\n"
          "\n"
          "void Wall::build() {}\n");
    CHECK(notes.empty());
    const std::string divided = "struct Door\n"
                                "{\n"
                                "  // --- Opening ---\n"
                                "  void open();\n"
                                "  void close();\n"
                                "};\n";
    CHECK(classOrder(divided, notes) == divided);
    CHECK(mentions(notes, "divided into parts"));
  }

  TEST("the safety check should compare non-blank lines, labels aside")
  {
    CHECK(sameLines("a\nb\n\nc\n", "c\n\na\n\n\nb\n"));
    CHECK(sameLines("  int x;  \n", "  int x;\n"));
    CHECK(sameLines(
        "class A\n{\n  int x;\n};\n", "class A\n{\npublic:\n  int x;\n};\n"));
    CHECK(!sameLines("a\nb\n", "a\n"));
    CHECK(!sameLines("a\nb\n", "a\nb\nb\n"));
    CHECK(!sameLines("a\nb\n", "a b\n"));
    CHECK(!sameLines("  a\n", "a\n"));
  }

  TEST("line edits should turn the old text into the new")
  {
    const auto apply = [](std::string text, const std::vector<LineEdit>& edits)
    {
      for (auto it = edits.rbegin(); it != edits.rend(); ++it)
        text.replace(it->from, it->to - it->from, it->text);

      return text;
    };
    CHECK(lineEdits("a\nb\n", "a\nb\n").empty());
    const std::vector<LineEdit> one = lineEdits("a\nb\nc\n", "a\n\nb\nc\n");
    CHECK(one.size() == 1u);

    if (one.size() == 1u)
    {
      CHECK(one[0].from == 2u);
      CHECK(one[0].to == 2u);
      CHECK(one[0].text == "\n");
    }

    CHECK(apply("a\nb", lineEdits("a\nb", "a\nb\n")) == "a\nb\n");
    CHECK(apply("x\ny\n", lineEdits("x\ny\n", "y\nx")) == "y\nx");
    std::mt19937 random(7);

    for (int round = 0; round < 200; ++round)
    {
      std::string before;
      std::string after;

      for (int line = 0; line < 30; ++line)
      {
        const std::string text(1, static_cast<char>('a' + random() % 5));

        if (random() % 4 != 0)
          before += text + "\n";

        if (random() % 4 != 0)
          after += text + "\n";
      }

      const std::vector<LineEdit> edits = lineEdits(before, after);
      CHECK(apply(before, edits) == after);

      for (std::size_t i = 1; i < edits.size(); ++i)
        CHECK(edits[i - 1].to <= edits[i].from);
    }
  }

  TEST("a process should get its input and give back its output")
  {
#if !defined(_WIN32)
    const std::string big(3 * 1024 * 1024, 'x');
    const ProcessResult echoed =
        runProcess({"cat"}, big, std::chrono::milliseconds(20000));
    CHECK(echoed.started);
    CHECK(echoed.exitCode == 0);
    CHECK(echoed.output == big);
    const ProcessResult failed = runProcess(
        {"sh", "-c", "echo oops >&2; exit 3"}, "", std::chrono::seconds(20));
    CHECK(failed.exitCode == 3);
    CHECK(failed.errors == "oops\n");
    const ProcessResult missing =
        runProcess({"sfs-no-such-program"}, "x", std::chrono::seconds(5));
    CHECK(!missing.started);
    const ProcessResult slow =
        runProcess({"sleep", "5"}, "", std::chrono::milliseconds(200));
    CHECK(slow.timedOut);
#endif
  }

  TEST(".sfs-format should be found above a file and read")
  {
    const HouseStyle style = houseStyleFor(house / "deep" / "a.cpp");
    CHECK(style.on);
    CHECK(style.root == house);
    CHECK(!style.sortFields);
    CHECK(style.exclude == std::vector<std::string>{"vendor"});
    CHECK(isExcluded(style, house / "vendor" / "lib.cpp"));
    CHECK(!isExcluded(style, house / "vendors.cpp"));
    CHECK(!houseStyleFor(scratch() / "plain" / "a.cpp").on);
    const HouseStyle odd =
        parseHouseStyle("sort-fields: yes\ncolour: blue\nexclude: a/, b\n");
    CHECK(odd.problems.size() == 2u);
    CHECK(odd.exclude == std::vector<std::string>{"a", "b"});
  }

  TEST("without .sfs-format only clang-format should run, and excluded files "
       "not at all")
  {
    if (identity().empty())
      break;

    const std::string text = fixture("blankLines.cpp.txt");
    CHECK(formatted(scratch() / "plain" / "a.cpp", text).text == text);
    const FormatResult skipped = formatted(house / "vendor" / "a.cpp", text);
    CHECK(skipped.excluded);
    CHECK(skipped.text == text);
    FormatRequest request;
    request.text = text;
    request.path = house / "a.cpp";
    request.clangFormat = (scratch() / "no-such-clang-format").string();
    const FormatResult alone = formatSource(request, lspTest::parser());
    CHECK(alone.noClangFormat);
    CHECK(alone.text == fixture("blankLines.cpp.expected.txt"));
  }

  TEST("line breaks should be kept: CRLF, and none at the end")
  {
    if (identity().empty())
      break;

    const std::string crlf = "void f()\r\n{\r\n  int x = 0;\r\n  if (x)\r\n    "
                             "x = 1;\r\n  x = 2;\r\n}";
    CHECK(
        formatted(house / "crlf.cpp", crlf).text ==
        "void f()\r\n{\r\n  int x = 0;\r\n\r\n  if (x)\r\n    x = 1;\r\n\r\n  "
        "x = 2;\r\n}");
  }

  TEST("range formatting should add blank lines only in the range, and not "
       "reorder")
  {
    if (identity().empty())
      break;

    const std::string text = "void Spawner::b()\n"
                             "{\n"
                             "  int x = 0;\n"
                             "  if (x)\n"
                             "    x = 1;\n"
                             "  x = 2;\n"
                             "  if (x)\n"
                             "    x = 3;\n"
                             "}\n"
                             "\n"
                             "Spawner::Spawner() {}\n";
    const FormatResult got = formatted(house / "range.cpp", text, 3, 5);
    CHECK(got.text == "void Spawner::b()\n"
                      "{\n"
                      "  int x = 0;\n"
                      "\n"
                      "  if (x)\n"
                      "    x = 1;\n"
                      "\n"
                      "  x = 2;\n"
                      "  if (x)\n"
                      "    x = 3;\n"
                      "}\n"
                      "\n"
                      "Spawner::Spawner() {}\n");
  }

  TEST("--format should rewrite files, and --format --check list them")
  {
    if (identity().empty())
      break;

    const std::filesystem::path tree = scratch() / "cli";
    lspTest::writeFile(tree / ".sfs-format", "exclude: vendor\n");
    lspTest::writeFile(
        tree / "src" / "playerSystem.h", fixture("playerSystem.h.txt"));
    lspTest::writeFile(tree / "src" / "tidy.cpp", "int one() { return 1; }\n");
    lspTest::writeFile(
        tree / "vendor" / "lib.cpp", fixture("blankLines.cpp.txt"));
    const std::vector<std::string> check = {
        "--check", "--clang-format", identity(), tree.string()};
    CHECK(runFormat(check) == 1);
    CHECK(readText(tree / "src" / "playerSystem.h") ==
          fixture("playerSystem.h.txt"));
    CHECK(runFormat({"--clang-format", identity(), tree.string()}) == 0);
    CHECK(readText(tree / "src" / "playerSystem.h") ==
          fixture("playerSystem.h.expected.txt"));
    CHECK(readText(tree / "vendor" / "lib.cpp") ==
          fixture("blankLines.cpp.txt"));
    CHECK(runFormat(check) == 0);
    CHECK(runFormat({"--clang-format",
                     (tree / "no-such-clang-format").string(),
                     tree.string()}) == 2);
    CHECK(runFormat({"--check"}) == 2);
  }

  TEST("with clang-format, formatting twice should change nothing")
  {
    const std::string clangFormat = findClangFormat("");

    if (clangFormat.empty())
    {
      std::cerr << "  (no clang-format: set SFS_CLANG_FORMAT to test it)\n";
      break;
    }

    const std::filesystem::path tree = scratch() / "real";
    lspTest::writeFile(tree / ".clang-format",
                       "BasedOnStyle: LLVM\n"
                       "BreakBeforeBraces: Allman\n"
                       "ColumnLimit: 120\n");
    lspTest::writeFile(tree / ".sfs-format", "\n");
    lspTest::writeFile(tree / "playerSystem.h", fixture("playerSystem.h.txt"));

    for (const auto& [name, input] :
         {std::pair<std::string, std::string>{
              "playerSystem.h", "playerSystem.h.txt"},
          {"playerSystem.cpp", "playerSystem.cpp.txt"},
          {"blankLines.cpp", "blankLines.cpp.txt"}})
    {
      FormatRequest request;
      const std::string text = fixture(input);
      request.text = text;
      request.path = tree / name;
      request.clangFormat = clangFormat;
      const FormatResult once = formatSource(request, lspTest::parser());
      request.text = once.text;
      const FormatResult twice = formatSource(request, lspTest::parser());
      CHECK(!once.clangFormatFailed);
      CHECK(!once.refused);
      CHECK(twice.text == once.text);
    }
  }

  std::error_code error;
  std::filesystem::remove_all(scratch(), error);
  return testing::report("lspFormatTests");
}
