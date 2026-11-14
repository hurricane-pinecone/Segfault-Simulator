#include "../testHarness.h"
#include "lspTestSupport.h"

#include <sfs/lsp/protocol.h>

#include <random>
#include <sstream>

using namespace sfs::lsp;

namespace
{

// The text of a large engine source, for the random edit test.
std::string engineSource()
{
  std::ifstream in(SFS_LSP_TEST_SOURCE, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string treeString(const Document& document)
{
  char* text = ts_node_string(ts_tree_root_node(document.tree.get()));
  std::string out(text);
  std::free(text);
  return out;
}

} // namespace

int main()
{
  TEST("UTF-8, UTF-16 and UTF-32 positions should convert to bytes and back")
  {
    // "a" (1 byte), "é" (2 bytes), "€" (3 bytes), "😀" (4 bytes: 2 UTF-16
    // units), "b".
    const std::string line = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                             "b";
    CHECK(characterToByte(line, 3, PositionEncoding::Utf8) == 3u);
    CHECK(characterToByte(line, 3, PositionEncoding::Utf16) == 6u);
    CHECK(characterToByte(line, 5, PositionEncoding::Utf16) == 10u);
    CHECK(characterToByte(line, 4, PositionEncoding::Utf32) == 10u);
    CHECK(characterToByte(line, 99, PositionEncoding::Utf16) == 11u);
    CHECK(byteToCharacter(line, 10, PositionEncoding::Utf16) == 5u);
    CHECK(byteToCharacter(line, 10, PositionEncoding::Utf32) == 4u);
    CHECK(byteToCharacter(line, 11, PositionEncoding::Utf8) == 11u);
    CHECK(byteToCharacter("plain", 3, PositionEncoding::Utf16) == 3u);
  }

  TEST("URIs should convert to paths and back")
  {
    CHECK(uriToPath("file:///home/me/a%20b.h") ==
          std::filesystem::path("/home/me/a b.h"));
    CHECK(uriToPath("file:///c:/game/x.h") ==
          std::filesystem::path("c:/game/x.h"));
    CHECK(uriToPath("http://x/y").empty());
    CHECK(pathToUri("/home/me/a b.h") == "file:///home/me/a%20b.h");
    CHECK(uriToPath(pathToUri("/odd/[x]#y.h")) ==
          std::filesystem::path("/odd/[x]#y.h"));
  }

  TEST("framed messages should be cut out of any split, and oversized ones "
       "skipped")
  {
    MessageReader reader;
    std::vector<std::string> messages;
    std::vector<std::string> problems;
    const std::string stream = frame("{\"a\":1}") + frame("{\"b\":2}");
    for (const char c : stream)
      reader.feed(std::string_view(&c, 1), messages, problems);
    CHECK(messages.size() == 2u);
    CHECK(messages.size() == 2u && messages[1] == "{\"b\":2}");
    messages.clear();
    reader.feed("Content-Length: 99999999999\r\n\r\nxx" + frame("{}"),
                messages,
                problems);
    CHECK(messages.empty()); // the rest is taken as the skipped body
    CHECK(problems.size() == 1u);
    MessageReader fresh;
    fresh.feed("X-Other: 1\r\n\r\n" + frame("{}"), messages, problems);
    CHECK(messages.size() == 1u);
  }

  TEST("edits across lines should keep the text and line starts right")
  {
    auto doc = lspTest::document("one\ntwo\nthree\n");
    applyEdit(*doc, {0, 1}, {2, 2}, "X\nY", PositionEncoding::Utf8);
    CHECK(doc->text == "oX\nYree\n");
    CHECK(doc->lineStarts == countLines(doc->text));
    applyEdit(*doc, {1, 4}, {1, 4}, "\n\nnew", PositionEncoding::Utf8);
    CHECK(doc->text == "oX\nYree\n\nnew\n");
    CHECK(doc->lineStarts == countLines(doc->text));
    applyEdit(*doc, {0, 0}, {4, 0}, "", PositionEncoding::Utf8);
    CHECK(doc->text.empty());
    CHECK(doc->lineStarts == countLines(doc->text));
    CHECK(doc->stale && doc->dirty);
  }

  TEST("UTF-16 edit positions over non-ASCII text should land on the right "
       "bytes")
  {
    auto doc = lspTest::document("// \xF0\x9F\x98\x80 x\nint a;\n");
    // After the emoji (2 UTF-16 units) and a space: character 6 is "x".
    applyEdit(*doc, {0, 6}, {0, 7}, "y", PositionEncoding::Utf16);
    CHECK(doc->text == "// \xF0\x9F\x98\x80 y\nint a;\n");
    CHECK(positionOf(*doc, 0, 8, PositionEncoding::Utf16).character == 6u);
  }

  TEST("random edits should leave the incrementally reparsed tree equal to a "
       "fresh parse")
  {
    const std::string source = engineSource();
    CHECK(!source.empty());
    auto doc = lspTest::document(source);
    std::mt19937 random(7);
    const std::string pieces[] = {
        "x", "\n", "{", "}", "int y;", "  ", "/*", "*/", "\"", "<", ">", "::"};
    bool linesRight = true;
    for (int i = 0; i < 1000; ++i)
    {
      const uint32_t lines = static_cast<uint32_t>(doc->lineStarts.size());
      const Position start{static_cast<uint32_t>(random() % lines),
                           static_cast<uint32_t>(random() % 40)};
      Position end = start;
      if (random() % 2)
        end = {std::min(
                   lines - 1, start.line + static_cast<uint32_t>(random() % 3)),
               static_cast<uint32_t>(random() % 40)};
      const uint32_t from = byteOffset(*doc, start, PositionEncoding::Utf8);
      const uint32_t to = byteOffset(*doc, end, PositionEncoding::Utf8);
      if (to < from)
        end = start;
      applyEdit(*doc,
                start,
                end,
                random() % 3 ? pieces[random() % std::size(pieces)] : "",
                PositionEncoding::Utf8);
      linesRight = linesRight && doc->lineStarts == countLines(doc->text);
      // Reparse now and then, as a request or a settle would.
      if (i % 50 == 49)
        reparse(*doc, lspTest::parser());
    }
    CHECK(linesRight);
    reparse(*doc, lspTest::parser());
    const auto fresh = lspTest::document(doc->text);
    CHECK(treeString(*doc) == treeString(*fresh));
  }

  return testing::report("lspDocumentTests");
}
