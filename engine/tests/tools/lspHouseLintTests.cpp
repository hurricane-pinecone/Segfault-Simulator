#include "../testHarness.h"
#include "houseLints.h"
#include "lintConfig.h"
#include "lspTestSupport.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace sfs::lsp;
using sfs::codegen::RuleId;

namespace
{

// A temporary tree whose root holds a .sfs-lint.
struct Tree
{
  std::filesystem::path root;

  explicit Tree(std::string_view config, std::string_view name = "sfsLspHouse")
      : root(std::filesystem::temp_directory_path() / name)
  {
    std::filesystem::remove_all(root);
    lspTest::writeFile(root / ".sfs-lint", config);
  }

  ~Tree() { std::filesystem::remove_all(root); }
};

// The tree the lint tests share, with the house rules on.
const Tree& houseTree()
{
  static const Tree tree(
      "# House style.\ninclude: house\n", "sfsLspHouseRules");
  return tree;
}

// `text` analysed as a file of the house tree.
std::unique_ptr<Document> house(std::string text,
                                std::string_view name = "file.cpp")
{
  return lspTest::analysed(std::move(text), houseTree().root / name);
}

// The text with the first fix of `rule` applied; the text itself without one.
std::string fixed(const Document& document, RuleId rule)
{
  const sfs::codegen::Diagnostic* diagnostic = lspTest::first(document, rule);
  return diagnostic ? lspTest::applyEdits(document.text, diagnostic->fix)
                    : document.text;
}

// Whether two texts are the same; prints both when they aren't.
bool sameText(const std::string& actual, const std::string& expected)
{
  if (actual != expected)
    std::printf("      got:\n%s\n      wanted:\n%s\n",
                actual.c_str(),
                expected.c_str());
  return actual == expected;
}

// Whether the first diagnostic of `rule` has a fix.
bool hasFix(const Document& document, RuleId rule)
{
  const sfs::codegen::Diagnostic* diagnostic = lspTest::first(document, rule);
  return diagnostic && !diagnostic->fix.empty();
}

std::string readText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// The text between a docs page's `<!-- name:begin -->` and `:end` lines.
std::string docsBlock(const std::filesystem::path& page, std::string_view name)
{
  const std::string text = readText(page);
  const std::string begin = "<!-- " + std::string(name) + ":begin -->\n";
  const std::string end = "<!-- " + std::string(name) + ":end -->";
  const std::size_t from = text.find(begin);
  const std::size_t to = text.find(end);
  if (from == std::string::npos || to == std::string::npos || to < from)
    return {};
  return text.substr(from + begin.size(), to - from - begin.size());
}

constexpr std::string_view NESTED = "int pick(int a, int b)\n"
                                    "{\n"
                                    "  return a ? (b ? 1 : 2) : 3;\n"
                                    "}\n";

} // namespace

int main()
{
  TEST("a .sfs-lint should be read as key: value lines")
  {
    std::vector<std::string> warnings;
    const LintConfig config =
        parseLintConfig("# The house style.\n"
                        "include: house  # on\n"
                        "\n"
                        "disable: nested-ternary, else-after-return\n"
                        "disable: own-entity\n",
                        "/t/.sfs-lint",
                        warnings);
    CHECK(config.house);
    CHECK(
        config.disabled.test(static_cast<std::size_t>(RuleId::NestedTernary)));
    CHECK(config.disabled.test(
        static_cast<std::size_t>(RuleId::ElseAfterReturn)));
    CHECK(config.disabled.test(static_cast<std::size_t>(RuleId::OwnEntity)));
    CHECK(
        !config.disabled.test(static_cast<std::size_t>(RuleId::ConstexprName)));
    CHECK(warnings.empty());

    const LintConfig empty = parseLintConfig("# nothing\n", "/t/x", warnings);
    CHECK(!empty.house);
    CHECK(empty.disabled.none());
    CHECK(warnings.empty());
  }

  TEST("a .sfs-lint should warn about what it doesn't know, and go on")
  {
    std::vector<std::string> warnings;
    const LintConfig config = parseLintConfig("colour: blue\n"
                                              "include: house, extras\n"
                                              "disable: nope, duplicate-type\n"
                                              "just words\n"
                                              "disable: own-entity\n",
                                              "/t/.sfs-lint",
                                              warnings);
    CHECK(config.house);
    CHECK(config.disabled.test(static_cast<std::size_t>(RuleId::OwnEntity)));
    CHECK(
        !config.disabled.test(static_cast<std::size_t>(RuleId::DuplicateType)));
    CHECK(warnings.size() == 5u);
    if (warnings.size() == 5u)
    {
      CHECK(warnings[0] == "/t/.sfs-lint:1: unknown key 'colour'");
      CHECK(warnings[1] == "/t/.sfs-lint:2: unknown rule set 'extras'");
      CHECK(warnings[2] == "/t/.sfs-lint:3: unknown rule 'nope'");
      CHECK(warnings[3] == "/t/.sfs-lint:3: duplicate-type can't be disabled");
      CHECK(warnings[4] ==
            "/t/.sfs-lint:4: expected 'key: value', got 'just words'");
    }
  }

  TEST("the nearest .sfs-lint above a file should win")
  {
    const Tree tree("include: house\n", "sfsLspHouseNearest");
    lspTest::writeFile(tree.root / "lib/.sfs-lint", "# Not ours.\n");
    lspTest::writeFile(tree.root / "src/deep/.sfs-lint",
                       "include: house\ndisable: nested-ternary\n");
    CHECK(findLintConfig(tree.root / "a.cpp") == tree.root / ".sfs-lint");
    CHECK(findLintConfig(tree.root / "src/b.cpp") == tree.root / ".sfs-lint");
    CHECK(findLintConfig(tree.root / "lib/c.cpp") ==
          tree.root / "lib/.sfs-lint");
    CHECK(findLintConfig(tree.root / "src/deep/d/e.cpp") ==
          tree.root / "src/deep/.sfs-lint");
    CHECK(findLintConfig("/t/file.cpp").empty());

    const std::string text(NESTED);
    CHECK(lspTest::count(*lspTest::analysed(text, tree.root / "a.cpp"),
                         RuleId::NestedTernary) == 1u);
    CHECK(lspTest::count(*lspTest::analysed(text, tree.root / "lib/c.cpp"),
                         RuleId::NestedTernary) == 0u);
    CHECK(
        lspTest::count(*lspTest::analysed(text, tree.root / "src/deep/d/e.cpp"),
                       RuleId::NestedTernary) == 0u);
    CHECK(!lintConfigFor(tree.root / "lib/c.cpp").house);
  }

  TEST("without a .sfs-lint the house rules should be off")
  {
    const std::string text = "constexpr int kMax = 3;\n" + std::string(NESTED) +
                             "int f(int a)\n{\n  if (a)\n    return 1;\n  "
                             "else\n    return 2;\n}\n";
    const auto plain = lspTest::analysed(text);
    CHECK(lspTest::count(*plain, RuleId::ConstexprName) == 0u);
    CHECK(lspTest::count(*plain, RuleId::NestedTernary) == 0u);
    CHECK(lspTest::count(*plain, RuleId::ElseAfterReturn) == 0u);
    const auto on = house(text);
    CHECK(lspTest::count(*on, RuleId::ConstexprName) == 1u);
    CHECK(lspTest::count(*on, RuleId::NestedTernary) == 1u);
    CHECK(lspTest::count(*on, RuleId::ElseAfterReturn) == 1u);
  }

  TEST("a .sfs-lint's edits should be seen at the next analysis")
  {
    const Tree tree("include: house\n", "sfsLspHouseEdited");
    const std::string text(NESTED);
    const std::filesystem::path file = tree.root / "a.cpp";
    CHECK(lspTest::count(
              *lspTest::analysed(text, file), RuleId::NestedTernary) == 1u);
    // A longer text, so the change shows even within the clock's tick.
    lspTest::writeFile(
        tree.root / ".sfs-lint", "include: house\ndisable: nested-ternary\n");
    CHECK(lspTest::count(
              *lspTest::analysed(text, file), RuleId::NestedTernary) == 0u);
    std::filesystem::remove(tree.root / ".sfs-lint");
    CHECK(!lintConfigFor(file).house);
  }

  TEST("disabled rules and sfs-ignore should still hide house warnings")
  {
    sfs::codegen::RuleSet disabled;
    disabled.set(static_cast<std::size_t>(RuleId::NestedTernary));
    CHECK(lspTest::count(*lspTest::analysed(std::string(NESTED),
                                            houseTree().root / "file.cpp",
                                            nullptr,
                                            disabled),
                         RuleId::NestedTernary) == 0u);
    const auto ignored =
        house("// sfs-ignore-file(nested-ternary)\n" + std::string(NESTED));
    CHECK(lspTest::count(*ignored, RuleId::NestedTernary) == 0u);
    CHECK(lspTest::count(*ignored, RuleId::BadIgnore) == 0u);
  }

  TEST("upperSnakeName should drop a k prefix and split words")
  {
    CHECK(upperSnakeName("kGateOpenDistance") == "GATE_OPEN_DISTANCE");
    CHECK(upperSnakeName("kHTTPPort") == "HTTP_PORT");
    CHECK(upperSnakeName("maxItems") == "MAX_ITEMS");
    CHECK(upperSnakeName("kMaxHP") == "MAX_HP");
    CHECK(upperSnakeName("kUI") == "UI");
    CHECK(upperSnakeName("kVec3Size") == "VEC3_SIZE");
    CHECK(upperSnakeName("kMax2DSize") == "MAX2D_SIZE");
    CHECK(upperSnakeName("texture2D") == "TEXTURE2D");
    CHECK(upperSnakeName("kHTTP2Port") == "HTTP2_PORT");
    CHECK(upperSnakeName("max_items") == "MAX_ITEMS");
    CHECK(upperSnakeName("Max__Items") == "MAX_ITEMS");
    CHECK(upperSnakeName("kilo") == "KILO");
    CHECK(upperSnakeName("k2D") == "K2D");
    CHECK(upperSnakeName("kMAX") == "MAX");
    CHECK(upperSnakeName("x") == "X");
  }

  TEST("constexpr-name should flag constexpr variables not in "
       "UPPER_SNAKE_CASE")
  {
    const auto doc = house("constexpr int kMax = 3;\n"
                           "static constexpr float maxSpeed{2.f};\n"
                           "constexpr int MAX_ITEMS = 4, LIMIT_2 = 5;\n"
                           "const int kNotConstexpr = 1;\n"
                           "constexpr int twice() { return 2; }\n"
                           "constexpr int S::kOutOfLine;\n"
                           "struct S\n{\n  static constexpr int kSize = 4;\n"
                           "  constexpr int f() const { return 1; }\n};\n"
                           "void g()\n{\n  constexpr auto kLocal = 5;\n"
                           "  if constexpr (true) {}\n}\n"
                           "template <typename T>\nconstexpr T kPi = T(3);\n");
    CHECK(lspTest::count(*doc, RuleId::ConstexprName) == 5u);
    std::vector<std::string> flagged;
    for (const sfs::codegen::Diagnostic& d : doc->diagnostics)
      if (d.rule == RuleId::ConstexprName)
        flagged.push_back(doc->text.substr(
            countLines(doc->text)[d.range.startRow] + d.range.startColumn,
            d.range.endColumn - d.range.startColumn));
    CHECK(flagged == std::vector<std::string>{
                         "kMax", "maxSpeed", "kSize", "kLocal", "kPi"});
    const sfs::codegen::Diagnostic* first =
        lspTest::first(*doc, RuleId::ConstexprName);
    CHECK(first && first->message ==
                       "A `constexpr` is named in UPPER_SNAKE_CASE: `kMax` "
                       "would be `MAX`.");
    CHECK(first && first->fixTitle == "Rename to MAX");
  }

  TEST("constexpr-name's fix should rename the constant and its uses")
  {
    const auto doc = house("namespace\n{\nconstexpr int kMaxItems = 3;\n}\n"
                           "int Box::count() const\n{\n"
                           "  // kMaxItems is the cap.\n"
                           "  return std::min(m_count, kMaxItems) + "
                           "f<kMaxItems>();\n}\n");
    CHECK(sameText(fixed(*doc, RuleId::ConstexprName),
                   "namespace\n{\nconstexpr int MAX_ITEMS = 3;\n}\n"
                   "int Box::count() const\n{\n"
                   "  // kMaxItems is the cap.\n"
                   "  return std::min(m_count, MAX_ITEMS) + "
                   "f<MAX_ITEMS>();\n}\n"));

    // A block's constant is renamed only within its block.
    const auto block = house("void a()\n{\n  constexpr int kStep = 2;\n"
                             "  run(kStep);\n}\n"
                             "void b(int kStep) { run(kStep); }\n");
    CHECK(sameText(fixed(*block, RuleId::ConstexprName),
                   "void a()\n{\n  constexpr int STEP = 2;\n"
                   "  run(STEP);\n}\n"
                   "void b(int kStep) { run(kStep); }\n"));
    // Renaming it again changes nothing: it's no longer reported.
    const auto again = house(fixed(*block, RuleId::ConstexprName));
    CHECK(lspTest::count(*again, RuleId::ConstexprName) == 0u);

    // A static constant at namespace scope, and one in a named namespace of
    // a source file.
    const auto named = house("namespace game\n{\nstatic constexpr int "
                             "kLimit = 1;\nint f() { return kLimit; }\n}\n");
    CHECK(sameText(fixed(*named, RuleId::ConstexprName),
                   "namespace game\n{\nstatic constexpr int "
                   "LIMIT = 1;\nint f() { return LIMIT; }\n}\n"));

    // Members, and names another scope qualifies, aren't the constant's.
    const auto others = house("constexpr int kTile = Map::kTile;\n"
                              "int f(P p) { return p.kTile + kTile; }\n");
    CHECK(sameText(fixed(*others, RuleId::ConstexprName),
                   "constexpr int TILE = Map::kTile;\n"
                   "int f(P p) { return p.kTile + TILE; }\n"));
    // A value the grammar reads as a type is renamed too.
    const auto vexing = house("void f()\n{\n  constexpr int kSize = 2;\n"
                              "  Buffer buffer(kSize);\n"
                              "  std::array<int, kSize> items;\n}\n");
    CHECK(sameText(fixed(*vexing, RuleId::ConstexprName),
                   "void f()\n{\n  constexpr int SIZE = 2;\n"
                   "  Buffer buffer(SIZE);\n"
                   "  std::array<int, SIZE> items;\n}\n"));
  }

  TEST("constexpr-name should leave the fix out where renaming isn't safe")
  {
    // The new name is taken in the file.
    const auto taken = house("constexpr int kMax = 3;\nint MAX = 4;\n");
    CHECK(!hasFix(*taken, RuleId::ConstexprName));
    const sfs::codegen::Diagnostic* takenDiagnostic =
        lspTest::first(*taken, RuleId::ConstexprName);
    CHECK(takenDiagnostic &&
          takenDiagnostic->message.find("already used") != std::string::npos);
    // Two constants would get one name.
    const auto twins =
        house("constexpr int kMax = 3;\nconstexpr int max = 4;\n");
    CHECK(lspTest::count(*twins, RuleId::ConstexprName) == 2u);
    for (const sfs::codegen::Diagnostic& d : twins->diagnostics)
      CHECK(d.fix.empty());
    // A name in a comment doesn't count as taken.
    const auto comment =
        house("// MAX is what kMax becomes.\nconstexpr int kMax = 3;\n");
    CHECK(hasFix(*comment, RuleId::ConstexprName));

    // A header's constant: its uses may be in other files.
    const auto header =
        house("#pragma once\nconstexpr int kMax = 3;\n", "Limits.h");
    CHECK(!hasFix(*header, RuleId::ConstexprName));
    const sfs::codegen::Diagnostic* headerDiagnostic =
        lspTest::first(*header, RuleId::ConstexprName);
    CHECK(headerDiagnostic &&
          headerDiagnostic->message ==
              "A `constexpr` is named in UPPER_SNAKE_CASE: `kMax` would be "
              "`MAX`. Rename it with clangd, which finds every use.");
    // A block's constant in a header has its fix.
    const auto inline_ = house("#pragma once\ninline int f()\n{\n"
                               "  constexpr int kTwo = 2;\n  return kTwo;\n}\n",
                               "Inline.h");
    CHECK(sameText(fixed(*inline_, RuleId::ConstexprName),
                   "#pragma once\ninline int f()\n{\n"
                   "  constexpr int TWO = 2;\n  return TWO;\n}\n"));
    // A class's constant, an extern one, and uses that can't be renamed: one
    // its own namespace qualifies, a member of that name, a macro, a use
    // above, another declaration.
    for (const char* text :
         {"struct S\n{\n  static constexpr int kSize = 4;\n};\n",
          "extern constexpr int kShared = 1;\n",
          "namespace a { constexpr int kX = 1; }\nint f() { return a::kX; }\n",
          "constexpr int kX = 1;\nstruct P { int kX; };\n",
          "constexpr int kX = 1;\n#define USE kX\n",
          "int g() { return kX; }\nconstexpr int kX = 1;\n",
          "constexpr int kX = 1;\nvoid f() { int kX = 2; }\n",
          "void f()\n{\n  constexpr int kX = 1;\n  { int kX = 2; }\n}\n"})
    {
      const auto doc = house(text);
      CHECK(lspTest::count(*doc, RuleId::ConstexprName) >= 1u);
      CHECK(!hasFix(*doc, RuleId::ConstexprName));
    }
  }

  TEST("nested-ternary should flag a ternary inside another")
  {
    const auto doc = house(std::string(NESTED));
    CHECK(lspTest::count(*doc, RuleId::NestedTernary) == 1u);
    const sfs::codegen::Diagnostic* d =
        lspTest::first(*doc, RuleId::NestedTernary);
    CHECK(d && d->range.startRow == 2u && d->range.startColumn == 14u);
    CHECK(d && d->fix.empty());
    CHECK(d && d->message ==
                   "Ternaries should not be nested: two are fine kept apart, "
                   "but not one inside another. Work the inner one out into "
                   "its own named variable first, and use that in the outer "
                   "ternary.");

    // In the condition, the false branch, or deeper inside a branch.
    const auto everywhere = house("int f(int a, int b)\n{\n"
                                  "  int x = (a ? b : 1) ? 2 : 3;\n"
                                  "  int y = a ? 1 : b ? 2 : 3;\n"
                                  "  int z = a ? g(b ? 1 : 2) : 3;\n"
                                  "  return x + y + z;\n}\n");
    CHECK(lspTest::count(*everywhere, RuleId::NestedTernary) == 3u);

    // Side by side, or apart through a variable, or in a lambda's body.
    const auto apart =
        house("int f(int a, int b)\n{\n"
              "  const int inner = b ? 1 : 2;\n"
              "  int x = a ? inner : 3;\n"
              "  int y = (a ? 1 : 2) + (b ? 3 : 4);\n"
              "  auto pick = a ? [](int c) { return c ? 1 : 2; } : nullptr;\n"
              "  return x + y;\n}\n");
    CHECK(lspTest::count(*apart, RuleId::NestedTernary) == 0u);
  }

  TEST("else-after-return should flag an else after a jump")
  {
    const auto doc = house("int f(int a)\n{\n"
                           "  if (a)\n    return 1;\n  else\n    g();\n"
                           "  for (;;)\n  {\n"
                           "    if (a) { g(); continue; } else { h(); }\n"
                           "    if (a) break; else h();\n"
                           "    if (a) throw 1; else h();\n"
                           "    if (a) { if (b) return 1; else return 2; } "
                           "else h();\n"
                           "  }\n  return 0;\n}\n");
    // The block's own if/else counts too.
    CHECK(lspTest::count(*doc, RuleId::ElseAfterReturn) == 6u);
    const sfs::codegen::Diagnostic* d =
        lspTest::first(*doc, RuleId::ElseAfterReturn);
    CHECK(d && d->range.startRow == 4u && d->range.startColumn == 2u &&
          d->range.endColumn == 6u);
    CHECK(d && d->message ==
                   "The `if` branch ends in `return`, so this `else` isn't "
                   "needed. Drop it and write its code after the `if`, so the "
                   "main path isn't nested.");

    // Not after a branch that may go on, nor for if constexpr, nor an if
    // that isn't in a block.
    const auto not_ = house("int f(int a)\n{\n"
                            "  if (a) { g(); } else { h(); }\n"
                            "  if (a) { if (b) return 1; } else h();\n"
                            "  if (a) { return 1; g(); } else h();\n"
                            "  if constexpr (A) return 1; else return 2;\n"
                            "  for (;;) if (a) break; else h();\n"
                            "  if (a) return 1;\n  h();\n  return 0;\n}\n");
    CHECK(lspTest::count(*not_, RuleId::ElseAfterReturn) == 0u);
  }

  TEST("else-after-return's fix should unnest the else's code")
  {
    // Braces on their own lines.
    const auto allman = house("int f(int a)\n{\n"
                              "  if (a)\n  {\n    return 1;\n  }\n"
                              "  else\n  {\n    g();\n\n    h(a,\n      a);\n"
                              "  }\n  return 0;\n}\n");
    CHECK(sameText(fixed(*allman, RuleId::ElseAfterReturn),
                   "int f(int a)\n{\n"
                   "  if (a)\n  {\n    return 1;\n  }\n"
                   "\n  g();\n\n  h(a,\n    a);\n"
                   "  return 0;\n}\n"));
    // Attached braces, and a four-space indent.
    const auto attached = house("int f(int a) {\n"
                                "    if (a) {\n        return 1;\n"
                                "    } else {\n        g();\n    }\n"
                                "    return 0;\n}\n");
    CHECK(sameText(fixed(*attached, RuleId::ElseAfterReturn),
                   "int f(int a) {\n"
                   "    if (a) {\n        return 1;\n    }\n"
                   "\n    g();\n"
                   "    return 0;\n}\n"));
    // No braces, a trailing comment on the branch, and a nested block.
    const auto plain = house("void f(int a)\n{\n  while (a)\n  {\n"
                             "    if (a > 2)\n      continue; // skip\n"
                             "    else\n      g();\n  }\n}\n");
    CHECK(sameText(fixed(*plain, RuleId::ElseAfterReturn),
                   "void f(int a)\n{\n  while (a)\n  {\n"
                   "    if (a > 2)\n      continue; // skip\n"
                   "\n    g();\n  }\n}\n"));
    // An else if: the chain follows unindented, and its own else is flagged
    // once the first is fixed.
    const auto chain = house("int f(int a)\n{\n"
                             "  if (a == 1)\n    return 1;\n"
                             "  else if (a == 2)\n    return 2;\n"
                             "  else\n    return 3;\n}\n");
    CHECK(lspTest::count(*chain, RuleId::ElseAfterReturn) == 1u);
    const std::string once = fixed(*chain, RuleId::ElseAfterReturn);
    CHECK(sameText(once,
                   "int f(int a)\n{\n"
                   "  if (a == 1)\n    return 1;\n\n"
                   "  if (a == 2)\n    return 2;\n"
                   "  else\n    return 3;\n}\n"));
    const auto twice = house(once);
    CHECK(sameText(fixed(*twice, RuleId::ElseAfterReturn),
                   "int f(int a)\n{\n"
                   "  if (a == 1)\n    return 1;\n\n"
                   "  if (a == 2)\n    return 2;\n\n"
                   "  return 3;\n}\n"));
    // A one-line else block, an empty one, and line breaks of \r\n.
    const auto oneLine =
        house("void f(int a)\n{\n  if (a) { return; } else { g(); }\n}\n");
    CHECK(sameText(fixed(*oneLine, RuleId::ElseAfterReturn),
                   "void f(int a)\n{\n  if (a) { return; }\n\n  g();\n}\n"));
    const auto empty =
        house("void f(int a)\n{\n  if (a)\n    return;\n  else\n  {\n  }\n}\n");
    CHECK(sameText(fixed(*empty, RuleId::ElseAfterReturn),
                   "void f(int a)\n{\n  if (a)\n    return;\n}\n"));
    const auto crlf = house("void f(int a)\r\n{\r\n  if (a)\r\n    return;"
                            "\r\n  else\r\n  {\r\n    g();\r\n  }\r\n}\r\n");
    CHECK(sameText(fixed(*crlf, RuleId::ElseAfterReturn),
                   "void f(int a)\r\n{\r\n  if (a)\r\n    return;"
                   "\r\n\r\n  g();\r\n}\r\n"));
    // Declarations that don't clash move with the code.
    const auto moved = house("int f(int a)\n{\n  if (a)\n    return 0;\n"
                             "  else\n  {\n    int b = a * 2;\n"
                             "    return b;\n  }\n}\n");
    CHECK(sameText(fixed(*moved, RuleId::ElseAfterReturn),
                   "int f(int a)\n{\n  if (a)\n    return 0;\n\n"
                   "  int b = a * 2;\n  return b;\n}\n"));
  }

  TEST("else-after-return's fix should be left out where it isn't safe")
  {
    for (const char* text : {
             // A declaration clashing with one in the block before the if.
             "void f(int a)\n{\n  int b = 1;\n  if (a)\n    return;\n"
             "  else\n  {\n    int b = 2;\n    g(b);\n  }\n}\n",
             // ... with a parameter.
             "void f(int a)\n{\n  if (a)\n    return;\n"
             "  else\n  {\n    int a = 2;\n    g(a);\n  }\n}\n",
             // ... with a loop's variable.
             "void f()\n{\n  for (int i = 0; i < 3; ++i)\n  {\n"
             "    if (i)\n      continue;\n    else\n    {\n"
             "      int i = 2;\n      g(i);\n    }\n  }\n}\n",
             // ... or changing what code after the if means.
             "int b;\nvoid f(int a)\n{\n  if (a)\n    return;\n"
             "  else\n  {\n    int b = 2;\n    g(b);\n  }\n  h(b);\n}\n",
             // The else uses the if's own declaration.
             "void f()\n{\n  if (int v = get(); v > 1)\n    return;\n"
             "  else\n    g(v);\n}\n",
             "void f()\n{\n  if (auto* p = find())\n    return;\n"
             "  else\n    g(p);\n}\n",
             // A comment on the else's line, before its block, or after it.
             "void f(int a)\n{\n  if (a)\n    return;\n"
             "  else // otherwise\n    g();\n}\n",
             "void f(int a)\n{\n  if (a) {\n    return;\n"
             "  } else { // otherwise\n    g();\n  }\n}\n",
             "void f(int a)\n{\n  if (a)\n    return;\n"
             "  // otherwise\n  else\n    g();\n}\n",
             "void f(int a)\n{\n  if (a)\n    return;\n"
             "  else\n  {\n    g();\n  } // done\n}\n",
             // A text that unindenting would change.
             "void f(int a)\n{\n  if (a)\n    return;\n"
             "  else\n  {\n    g(R\"(one\n    two)\");\n  }\n}\n",
             // In a case, where a declaration would cross the next label.
             "void f(int a)\n{\n  switch (a)\n  {\n  case 1:\n"
             "    if (b)\n      break;\n    else\n    {\n      int c = 1;\n"
             "      g(c);\n    }\n    break;\n  }\n}\n",
         })
    {
      const auto doc = house(text);
      CHECK(lspTest::count(*doc, RuleId::ElseAfterReturn) == 1u);
      CHECK(!hasFix(*doc, RuleId::ElseAfterReturn));
    }
    // An init-statement the else doesn't use is fine.
    const auto unused = house("void f()\n{\n  if (int v = get(); v > 1)\n"
                              "    return;\n  else\n    g();\n}\n");
    CHECK(hasFix(*unused, RuleId::ElseAfterReturn));
  }

  TEST("house rules should be tagged, last in the table, and in their own docs "
       "table")
  {
    using sfs::codegen::RuleInfo;
    const std::array<RuleId, 3> house = {
        RuleId::ConstexprName, RuleId::NestedTernary, RuleId::ElseAfterReturn};
    std::size_t at = static_cast<std::size_t>(RuleId::BadIgnore);
    for (const RuleId id : house)
    {
      const RuleInfo& rule = sfs::codegen::rules()[++at];
      CHECK(rule.id == id);
      CHECK(rule.house);
      CHECK(rule.source == sfs::codegen::RuleSource::Lint);
      CHECK(rule.severity == sfs::codegen::Severity::Warning);
      CHECK(sfs::codegen::canIgnore(id));
      CHECK(sfs::codegen::findRule(rule.name) == &rule);
      const std::string named = "`" + std::string(rule.name) + "`";
      CHECK(sfs::codegen::ruleDocsTable().find(named) == std::string::npos);
      CHECK(sfs::codegen::houseRuleDocsTable().find(named) !=
            std::string::npos);
    }
    CHECK(at + 1 == static_cast<std::size_t>(RuleId::Count));
    CHECK(sfs::codegen::ruleInfo(RuleId::ConstexprName).name ==
          "constexpr-name");
    CHECK(sfs::codegen::ruleInfo(RuleId::NestedTernary).name ==
          "nested-ternary");
    CHECK(sfs::codegen::ruleInfo(RuleId::ElseAfterReturn).name ==
          "else-after-return");
    for (const RuleInfo& rule : sfs::codegen::rules())
      if (rule.id < RuleId::ConstexprName)
        CHECK(!rule.house);

    const std::string block = docsBlock(
        std::filesystem::path(SFS_DOCS_DIR) / "editor/sfs-lsp/index.md",
        "sfs-house-rules");
    CHECK(block == sfs::codegen::houseRuleDocsTable());
    if (block != sfs::codegen::houseRuleDocsTable())
      std::fprintf(stderr,
                   "docs/editor/sfs-lsp/index.md should hold:\n%s",
                   sfs::codegen::houseRuleDocsTable().c_str());
  }

  TEST("--check and --fix should run the house rules where .sfs-lint has them")
  {
    const Tree tree("include: house\n", "sfsLspHouseCli");
    const std::filesystem::path source = tree.root / "src/file.cpp";
    lspTest::writeFile(source,
                       "constexpr int kMax = 3;\n" + std::string(NESTED) +
                           "int f(int a)\n{\n  if (a)\n    return kMax;\n"
                           "  else\n    return 2;\n}\n");
    const std::filesystem::path output = tree.root / "out.txt";
    const std::string run = "\"" SFS_LSP_PATH "\" ";
    // Warnings alone exit with 0.
    CHECK(std::system((run + "--check \"" + source.string() + "\" > \"" +
                       output.string() + "\"")
                          .c_str()) == 0);
    const std::string checked = readText(output);
    CHECK(checked.find("[constexpr-name]") != std::string::npos);
    CHECK(checked.find("[nested-ternary]") != std::string::npos);
    CHECK(checked.find("[else-after-return]") != std::string::npos);

    CHECK(std::system((run + "--fix constexpr-name,else-after-return \"" +
                       source.string() + "\" > \"" + output.string() + "\"")
                          .c_str()) == 0);
    CHECK(sameText(readText(source),
                   "constexpr int MAX = 3;\n" + std::string(NESTED) +
                       "int f(int a)\n{\n  if (a)\n    return MAX;\n\n"
                       "  return 2;\n}\n"));
    // Messages are plain sentences.
    const auto doc = lspTest::analysed(readText(source), source);
    for (const sfs::codegen::Diagnostic& d : doc->diagnostics)
      CHECK(!d.message.empty() && d.message.back() == '.');

    // Unknown keys are warned about on stderr, and the check goes on.
    lspTest::writeFile(tree.root / ".sfs-lint", "include: house\nsize: 3\n");
    const std::filesystem::path errors = tree.root / "errors.txt";
    CHECK(std::system((run + "--check \"" + source.string() + "\" > \"" +
                       output.string() + "\" 2> \"" + errors.string() + "\"")
                          .c_str()) == 0);
    CHECK(readText(errors).find(".sfs-lint:2: unknown key 'size'") !=
          std::string::npos);
    CHECK(readText(output).find("[nested-ternary]") != std::string::npos);
  }

  return testing::report("lspHouseLintTests");
}
