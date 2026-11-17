#include "formatOrder.h"

#include "formatLines.h"
#include "lints.h"
#include "lintSupport.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sfs::lsp
{

namespace
{

using format::field;
using format::firstRow;
using format::isType;
using format::lastRow;
using format::LineIndex;
using format::nodeText;
using format::parsesBadly;
using format::Rewrite;

// One thing in a body on whole lines: a declaration, an access label or
// something that stays put, with the comments above it and the one after it on
// its last line. Comments at the body's end make an entry with no node.
struct Entry
{
  TSNode node{};
  uint32_t first = 0;     // the first row, its leading comments'
  uint32_t nodeFirst = 0; // the node's own first row
  uint32_t last = 0;      // the last row, a trailing comment's
};

bool hasNode(const Entry& entry) { return !ts_node_is_null(entry.node); }

// A line's text for a note, as `text`: the formatted text's line numbers
// aren't the editor's.
std::string quoted(const LineIndex& lines, uint32_t row)
{
  constexpr std::size_t LONGEST = 60;
  std::string_view text = lint::trimmed(lines.line(row));
  std::string out = "`" + std::string(text.substr(0, LONGEST));
  return out + (text.size() > LONGEST ? "...`" : "`");
}

// Whether a comment is a divider between parts of a file (`// --- Name ---`,
// `// ===`): it stays where it is, and nothing moves across it.
bool isDivider(std::string_view comment)
{
  if (!comment.starts_with("//") && !comment.starts_with("/*"))
    return false;

  comment.remove_prefix(2);
  comment = lint::trimmed(comment);
  return comment.starts_with("---") || comment.starts_with("===");
}

// The children of a body (between `openRow` and `closeRow`, the rows of its
// braces; -1 and the row count for a whole file) as entries. Empty, or why it
// can't be split into whole lines.
std::string splitEntries(TSNode body,
                         const LineIndex& lines,
                         int64_t openRow,
                         int64_t closeRow,
                         std::vector<Entry>& out)
{
  bool pending = false;
  uint32_t pendingFirst = 0;
  uint32_t pendingLast = 0;
  const uint32_t count = ts_node_child_count(body);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_child(body, i);
    const std::string_view type = ts_node_type(child);
    const uint32_t row = firstRow(child);

    if (!ts_node_is_named(child))
    {
      if ((type == "{" && row == openRow) || (type == "}" && row == closeRow))
        continue;

      // A label's colon, or the semicolon after a class or enum.
      if ((type == ":" || type == ";") && !out.empty() && !pending &&
          hasNode(out.back()) && row == out.back().last)
        continue;

      return "an unexpected '" + std::string(type) + "' in " +
             quoted(lines, row);
    }

    if (type == "comment")
    {
      if (row == openRow)
        continue;

      if (!pending && !out.empty() && row == out.back().last)
      {
        out.back().last = std::max(out.back().last, lastRow(child));
        continue;
      }

      if (row >= closeRow ||
          !lines.startsAt(row, ts_node_start_point(child).column))
        return "a comment shares a line: " + quoted(lines, row);

      if (isDivider(nodeText(child, lines.text())))
      {
        out.push_back(
            {child, pending ? pendingFirst : row, row, lastRow(child)});
        pending = false;
        continue;
      }

      if (!pending)
        pendingFirst = row;

      pending = true;
      pendingLast = lastRow(child);
      continue;
    }

    if (static_cast<int64_t>(row) <= openRow ||
        static_cast<int64_t>(lastRow(child)) >= closeRow)
      return "a declaration shares a line with a brace: " + quoted(lines, row);

    if ((!out.empty() && row <= out.back().last) ||
        (pending && row <= pendingLast) ||
        !lines.startsAt(row, ts_node_start_point(child).column))
      return "two things share a line: " + quoted(lines, row);

    out.push_back({child, pending ? pendingFirst : row, row, lastRow(child)});
    pending = false;
  }

  if (pending)
    out.push_back({TSNode{}, pendingFirst, pendingFirst, pendingLast});

  return {};
}

// The function declarator a declarator declares, through pointers and
// references; null for a variable (a function pointer included).
TSNode functionDeclarator(TSNode declarator)
{
  while (!ts_node_is_null(declarator))
  {
    const std::string_view type = ts_node_type(declarator);

    if (type == "function_declarator")
      return isType(field(declarator, "declarator"), "parenthesized_declarator")
                 ? TSNode{}
                 : declarator;

    if (type == "operator_cast")
      return declarator;

    if (type != "pointer_declarator" && type != "reference_declarator" &&
        type != "attributed_declarator")
      return TSNode{};

    const TSNode inner = field(declarator, "declarator");

    if (!ts_node_is_null(inner))
    {
      declarator = inner;
      continue;
    }

    // A reference declarator names its inner declarator by position only.
    const uint32_t named = ts_node_named_child_count(declarator);

    if (named == 0)
      return TSNode{};

    declarator = ts_node_named_child(declarator, named - 1);
  }

  return TSNode{};
}

// The declarators of a declaration (its "declarator" fields).
std::vector<TSNode> declarators(TSNode node)
{
  std::vector<TSNode> found;
  const uint32_t count = ts_node_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
    if (const char* name = ts_node_field_name_for_child(node, i);
        name && std::string_view(name) == "declarator")
      found.push_back(ts_node_child(node, i));

  return found;
}

// A function declaration's name as it sorts: "update", "~System",
// "operator==", "operator bool".
std::string functionName(TSNode function, std::string_view text)
{
  if (isType(function, "operator_cast"))
    return "operator " + lint::tidy(nodeText(field(function, "type"), text));

  TSNode name = field(function, "declarator");

  while (isType(name, "qualified_identifier"))
    name = field(name, "name");

  if (isType(name, "template_function"))
    name = field(name, "name");

  if (isType(name, "operator_name"))
  {
    std::string spelled;

    for (const char c : nodeText(name, text))
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        spelled += c;

    return spelled;
  }

  return std::string(nodeText(name, text));
}

// The scope parts of a qualified function name (`a::B::f` gives a and B).
std::vector<std::string> scopeOf(TSNode function, std::string_view text)
{
  std::vector<std::string> parts;
  TSNode name = field(function, "declarator");

  while (isType(name, "qualified_identifier"))
  {
    TSNode scope = field(name, "scope");

    if (isType(scope, "template_type"))
      scope = field(scope, "name");

    parts.emplace_back(nodeText(scope, text));
    name = field(name, "name");
  }

  return parts;
}

// The first identifier in a subtree, depth first.
TSNode firstIdentifier(TSNode node)
{
  if (ts_node_is_null(node))
    return node;

  const std::string_view type = ts_node_type(node);

  if (type == "identifier" || type == "field_identifier" ||
      type == "type_identifier")
    return node;

  const uint32_t count = ts_node_named_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
    if (const TSNode found = firstIdentifier(ts_node_named_child(node, i));
        !ts_node_is_null(found))
      return found;

  return TSNode{};
}

// Whether a declaration says `static` (or `constexpr` and the like, `words`).
bool hasWord(TSNode node,
             std::string_view text,
             std::initializer_list<std::string_view> words)
{
  const uint32_t count = ts_node_named_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_named_child(node, i);
    const std::string_view type = ts_node_type(child);

    if (type != "storage_class_specifier" && type != "type_qualifier")
      continue;

    const std::string_view spelled = nodeText(child, text);

    if (std::find(words.begin(), words.end(), spelled) != words.end())
      return true;
  }

  return false;
}

// The names a subtree uses: its identifiers, type and namespace names (member
// names too). Function bodies and constructor initialisers are skipped when
// `signatureOnly`, as is the initialiser of a non-static data member.
void collectUses(TSNode node,
                 std::string_view text,
                 bool signatureOnly,
                 bool skipInitialiser,
                 std::unordered_set<std::string_view>& uses)
{
  const uint32_t count = ts_node_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_child(node, i);

    if (!ts_node_is_named(child))
      continue;

    const std::string_view type = ts_node_type(child);

    if (signatureOnly &&
        (type == "compound_statement" || type == "field_initializer_list"))
      continue;

    if (skipInitialiser)
      if (const char* name = ts_node_field_name_for_child(node, i);
          name && std::string_view(name) == "default_value")
        continue;

    if (type == "identifier" || type == "type_identifier" ||
        type == "namespace_identifier" || type == "field_identifier")
      uses.insert(nodeText(child, text));
    else
      collectUses(child, text, signatureOnly, false, uses);
  }
}

// What a case-insensitive alphabetical order compares.
std::string sortKey(std::string_view name)
{
  std::string key(name);

  for (char& c : key)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  return key;
}

// Rows `first` to `last` as they now read.
std::string rendered(const Rewrite& rewrite, uint32_t first, uint32_t last)
{
  std::string out;
  rewrite.render(first, last, out);
  return out;
}

// --- Pass (a): class bodies ---

enum class Access : uint8_t
{
  Public,
  Protected,
  Private,
};

enum class Group : uint8_t
{
  Head,
  Method,
  Field,
};

// One declaration of a class body.
struct Member
{
  const Entry* entry = nullptr;
  Group group = Group::Head;
  bool isStatic = false;
  int special = 3; // 0 constructor, 1 destructor, 2 assignment, 3 other
  std::string name;
  std::vector<std::string_view> declares; // names later declarations may use
  uint32_t index = 0;
  uint32_t section = 0;
};

// Comments at a section's end, before the next label or the closing brace.
struct Tail
{
  uint32_t first = 0;
  uint32_t last = 0;
  bool blankBefore = false;
};

// The access a label's keyword gives.
Access accessOf(std::string_view keyword)
{
  if (keyword == "public")
    return Access::Public;

  return keyword == "protected" ? Access::Protected : Access::Private;
}

// The label of an access.
std::string_view labelOf(Access access)
{
  switch (access)
  {
  case Access::Public:
    return "public:";
  case Access::Protected:
    return "protected:";
  case Access::Private:
    return "private:";
  }

  return "private:";
}

struct Section
{
  Access access = Access::Public;
  const Entry* label = nullptr; // null: the body's implicit first section
  std::vector<uint32_t> members;
  std::vector<Tail> tails;
};

// What a class member is; false when it's nothing the pass knows (a macro).
bool classify(TSNode node,
              std::string_view className,
              std::string_view text,
              Member& member)
{
  const std::string_view type = ts_node_type(node);

  if (type == "template_declaration")
  {
    const uint32_t named = ts_node_named_child_count(node);

    if (named < 2)
      return false;

    const TSNode inner = ts_node_named_child(node, named - 1);

    if (!classify(inner, className, text, member))
      return false;

    // A variable template is static.
    if (member.group == Group::Field)
      member.isStatic = true;

    return true;
  }

  if (type == "using_declaration" || type == "alias_declaration" ||
      type == "type_definition" || type == "friend_declaration" ||
      type == "static_assert_declaration")
  {
    member.group = Group::Head;

    if (type == "alias_declaration")
      member.declares.push_back(nodeText(field(node, "name"), text));
    else if (type == "type_definition")
    {
      for (const TSNode declarator : declarators(node))
        if (const TSNode name = firstIdentifier(declarator);
            !ts_node_is_null(name))
          member.declares.push_back(nodeText(name, text));
    }
    else if (type == "using_declaration")
      member.declares.push_back(lint::lastName(lint::trimmed(
          nodeText(node, text).substr(5, nodeText(node, text).size() - 6))));

    return true;
  }

  if (type != "field_declaration" && type != "declaration" &&
      type != "function_definition")
    return false;

  const std::vector<TSNode> found = declarators(node);

  if (found.empty())
  {
    // A nested type (or its forward declaration).
    const TSNode spec = field(node, "type");
    const std::string_view specType =
        ts_node_is_null(spec) ? std::string_view() : ts_node_type(spec);

    if (specType != "class_specifier" && specType != "struct_specifier" &&
        specType != "union_specifier" && specType != "enum_specifier")
      return false;

    member.group = Group::Head;

    if (const TSNode name = field(spec, "name"); !ts_node_is_null(name))
      member.declares.push_back(nodeText(name, text));

    // An unscoped enum's enumerators are names of the class.
    const TSNode values = field(spec, "body");

    if (specType == "enum_specifier" && !ts_node_is_null(values) &&
        ts_node_child_count(spec) > 1 &&
        ts_node_type(ts_node_child(spec, 1)) != std::string_view("class") &&
        ts_node_type(ts_node_child(spec, 1)) != std::string_view("struct"))
    {
      const uint32_t count = ts_node_named_child_count(values);

      for (uint32_t i = 0; i < count; ++i)
        if (const TSNode name = field(ts_node_named_child(values, i), "name");
            !ts_node_is_null(name))
          member.declares.push_back(nodeText(name, text));
    }

    return true;
  }

  // A type defined with a variable of it (`struct {...} m_x;`) is that
  // variable, and declares the type's name.
  if (const TSNode spec = field(node, "type");
      !ts_node_is_null(spec) && !ts_node_is_null(field(spec, "body")))
  {
    member.group = Group::Field;
    member.isStatic = hasWord(node, text, {"static"});

    if (const TSNode name = firstIdentifier(found.front());
        !ts_node_is_null(name))
      member.name = std::string(nodeText(name, text));

    if (const TSNode name = field(spec, "name"); !ts_node_is_null(name))
      member.declares.push_back(nodeText(name, text));

    return true;
  }

  if (const TSNode function = functionDeclarator(found.front());
      !ts_node_is_null(function))
  {
    member.group = Group::Method;
    member.name = functionName(function, text);
    const TSNode name = field(function, "declarator");

    if (isType(name, "destructor_name"))
      member.special = 1;
    else if (member.name == "operator=")
      member.special = 2;
    else if (member.name == className && ts_node_is_null(field(node, "type")))
      member.special = 0;

    return true;
  }

  member.group = Group::Field;
  member.isStatic = hasWord(node, text, {"static"});

  if (const TSNode name = firstIdentifier(found.front());
      !ts_node_is_null(name))
    member.name = std::string(nodeText(name, text));

  if (member.isStatic)
    for (const TSNode declarator : found)
      if (const TSNode name = firstIdentifier(declarator);
          !ts_node_is_null(name))
        member.declares.push_back(nodeText(name, text));

  return true;
}

class ClassPass
{
public:
  ClassPass(std::string_view text,
            const LineIndex& lines,
            Rewrite& rewrite,
            bool sortFields,
            std::vector<std::string>& notes)
      : m_text(text), m_lines(lines), m_rewrite(rewrite),
        m_sortFields(sortFields), m_notes(notes)
  {
  }

  // Orders one class body, recording its new text in the rewrite.
  void order(TSNode specifier, TSNode body);

private:
  std::string orderBody(TSNode specifier, TSNode body);

  std::string_view m_text;
  const LineIndex& m_lines;
  Rewrite& m_rewrite;
  bool m_sortFields;
  std::vector<std::string>& m_notes;
};

void ClassPass::order(TSNode specifier, TSNode body)
{
  const uint32_t openRow = firstRow(body);
  const uint32_t closeRow = ts_node_end_point(body).row;

  if (closeRow <= openRow + 1)
    return;

  const std::string why = orderBody(specifier, body);

  if (!why.empty())
  {
    const TSNode name = field(specifier, "name");
    m_notes.push_back((ts_node_is_null(name)
                           ? std::string("an unnamed class")
                           : "class " + std::string(nodeText(name, m_text))) +
                      " keeps its order: " + why);
  }
}

std::string ClassPass::orderBody(TSNode specifier, TSNode body)
{
  if (parsesBadly(body, false))
    return "it doesn't parse (tree-sitter misreads something in it)";

  const uint32_t openRow = firstRow(body);
  const uint32_t closeRow = ts_node_end_point(body).row;
  std::vector<Entry> entries;

  if (std::string why = splitEntries(body, m_lines, openRow, closeRow, entries);
      !why.empty())
    return why;

  if (entries.empty())
    return {};

  const bool isStruct = isType(specifier, "struct_specifier");
  const Access fallback = isStruct ? Access::Public : Access::Private;
  const std::string className =
      std::string(lint::lastName(nodeText(field(specifier, "name"), m_text)));

  std::vector<Member> members;
  std::vector<Section> sections;
  std::vector<Tail> lead; // comments above the first label
  sections.push_back({fallback, nullptr, {}, {}});
  uint32_t previousLast = openRow;

  for (const Entry& entry : entries)
  {
    const bool blankBefore = entry.first > previousLast + 1;

    if (!hasNode(entry))
    {
      sections.back().tails.push_back({entry.first, entry.last, blankBefore});
      previousLast = entry.last;
      continue;
    }

    if (isType(entry.node, "access_specifier"))
    {
      const std::string_view line =
          lint::trimmed(m_lines.line(entry.nodeFirst));
      const std::string_view access = nodeText(entry.node, m_text);

      if (entry.last != entry.nodeFirst || line != std::string(access) + ":")
        return "a label has a comment beside it";

      if (entry.first < entry.nodeFirst)
      {
        Tail comments{entry.first, entry.nodeFirst - 1, blankBefore};

        if (members.empty() && sections.size() == 1)
          lead.push_back(comments);
        else
          sections.back().tails.push_back(comments);
      }

      sections.push_back({accessOf(access), &entry, {}, {}});
      previousLast = entry.last;
      continue;
    }

    if (isType(entry.node, "comment"))
      return "it's divided into parts by " + quoted(m_lines, entry.nodeFirst);

    Member member;
    member.entry = &entry;
    member.index = static_cast<uint32_t>(members.size());
    member.section = static_cast<uint32_t>(sections.size() - 1);

    if (!classify(entry.node, className, m_text, member))
      return quoted(m_lines, entry.nodeFirst) +
             " isn't a declaration it can move (a macro?)";

    sections.back().members.push_back(member.index);
    members.push_back(std::move(member));
    previousLast = entry.last;
  }

  // The new order: by access, each access's sections merged; head, methods,
  // fields.
  struct Merged
  {
    Access access;
    const Entry* label = nullptr;
    std::array<std::vector<uint32_t>, 3> groups;
    std::vector<Tail> tails;
    std::vector<uint32_t> sources; // original sections merged into it
  };
  std::vector<Merged> merged;

  for (const Access access :
       {Access::Public, Access::Protected, Access::Private})
  {
    Merged into{access, nullptr, {}, {}, {}};

    for (uint32_t s = 0; s < sections.size(); ++s)
    {
      const Section& section = sections[s];

      if (section.access != access)
        continue;

      if (!into.label && section.label)
        into.label = section.label;

      into.sources.push_back(s);

      for (const uint32_t m : section.members)
        into.groups[static_cast<std::size_t>(members[m].group)].push_back(m);

      into.tails.insert(
          into.tails.end(), section.tails.begin(), section.tails.end());
    }

    if (into.sources.empty())
      continue;

    std::vector<uint32_t>& methods = into.groups[1];
    std::stable_sort(methods.begin(),
                     methods.end(),
                     [&](uint32_t a, uint32_t b)
                     {
                       const Member& x = members[a];
                       const Member& y = members[b];

                       if (x.special != y.special)
                         return x.special < y.special;

                       if (x.special < 3)
                         return false;

                       return sortKey(x.name) < sortKey(y.name);
                     });

    if (m_sortFields)
      std::stable_sort(
          into.groups[2].begin(),
          into.groups[2].end(),
          [&](uint32_t a, uint32_t b)
          { return sortKey(members[a].name) < sortKey(members[b].name); });

    if (into.groups[0].empty() && into.groups[1].empty() &&
        into.groups[2].empty() && into.tails.empty())
      continue;

    merged.push_back(std::move(into));
  }

  std::vector<uint32_t> newIndex(members.size());
  {
    uint32_t at = 0;

    for (const Merged& section : merged)
      for (const std::vector<uint32_t>& group : section.groups)
        for (const uint32_t m : group)
          newIndex[m] = at++;
  }

  // Without sortFields, the data members keep their order (layout,
  // initialisation, aggregates).
  if (!m_sortFields)
  {
    std::vector<uint32_t> before;
    std::vector<uint32_t> after;

    for (const Member& member : members)
      if (member.group == Group::Field && !member.isStatic)
        before.push_back(member.index);

    for (const Merged& section : merged)
      for (const uint32_t m : section.groups[2])
        if (!members[m].isStatic)
          after.push_back(m);

    if (before != after)
      return "its sections' new order would change the order of its fields";
  }

  // A declaration can't move above a name its type or signature uses.
  for (const Member& user : members)
  {
    std::unordered_set<std::string_view> uses;
    collectUses(user.entry->node,
                m_text,
                true,
                user.group == Group::Field && !user.isStatic,
                uses);

    for (const Member& declared : members)
    {
      if (&declared == &user || declared.index > user.index ||
          newIndex[declared.index] < newIndex[user.index])
        continue;

      for (const std::string_view name : declared.declares)
        if (uses.count(name))
          return quoted(m_lines, user.entry->nodeFirst) + " uses " +
                 std::string(name) + ", which would come after it";
    }
  }

  // Blank lines between new neighbours in a group: as most of its old
  // neighbours had them.
  const auto separated = [&](const Merged& section, std::size_t group)
  {
    int blank = 0;
    int pairs = 0;
    int sectionBlank = 0;
    int sectionPairs = 0;

    for (const uint32_t s : section.sources)
    {
      const std::vector<uint32_t>& order = sections[s].members;

      for (std::size_t i = 1; i < order.size(); ++i)
      {
        const Member& a = members[order[i - 1]];
        const Member& b = members[order[i]];
        const bool isBlank = b.entry->first > a.entry->last + 1;
        ++sectionPairs;
        sectionBlank += isBlank;

        if (static_cast<std::size_t>(a.group) == group &&
            static_cast<std::size_t>(b.group) == group)
        {
          ++pairs;
          blank += isBlank;
        }
      }
    }

    if (pairs > 0)
      return blank * 2 >= pairs;

    return sectionPairs > 0 && sectionBlank * 2 >= sectionPairs;
  };

  const std::string lineBreak(format::lineBreakOf(m_text));
  std::string labelIndent;

  for (const Section& section : sections)
    if (section.label)
    {
      labelIndent = std::string(m_lines.indent(section.label->nodeFirst));
      break;
    }

  if (labelIndent.empty())
    labelIndent = std::string(m_lines.indent(firstRow(specifier)));

  const bool startedImplicit = !sections.front().members.empty();

  std::string out;
  bool emitted = false;

  for (const Tail& comments : lead)
  {
    if (emitted && comments.blankBefore)
      out += lineBreak;

    m_rewrite.render(comments.first, comments.last, out);
    emitted = true;
  }

  const bool afterLead = emitted;

  for (std::size_t s = 0; s < merged.size(); ++s)
  {
    const Merged& section = merged[s];
    const bool labelled =
        !(s == 0 && section.access == fallback && startedImplicit);

    if (labelled)
    {
      if (emitted && !(s == 0 && afterLead))
        out += lineBreak;

      if (section.label)
        out += m_lines.line(section.label->nodeFirst);
      else
        out += labelIndent + std::string(labelOf(section.access)) + lineBreak;

      emitted = true;
    }

    bool groupDone = false;

    for (std::size_t g = 0; g < section.groups.size(); ++g)
    {
      const std::vector<uint32_t>& group = section.groups[g];

      if (group.empty())
        continue;

      if (groupDone || (!labelled && emitted))
        out += lineBreak;

      const bool blank = separated(section, g);

      for (std::size_t i = 0; i < group.size(); ++i)
      {
        // Neighbours that were neighbours keep their blank line or its lack.
        if (i > 0)
        {
          const Member& previous = members[group[i - 1]];
          const Member& member = members[group[i]];
          const bool kept = member.index == previous.index + 1 &&
                            member.section == previous.section;

          if (kept ? member.entry->first > previous.entry->last + 1 : blank)
            out += lineBreak;
        }

        const Entry& entry = *members[group[i]].entry;
        m_rewrite.render(entry.first, entry.last, out);
      }

      groupDone = true;
      emitted = true;
    }

    for (const Tail& comments : section.tails)
    {
      if (emitted && comments.blankBefore)
        out += lineBreak;

      m_rewrite.render(comments.first, comments.last, out);
      emitted = true;
    }
  }

  if (out != rendered(m_rewrite, openRow + 1, closeRow - 1))
    m_rewrite.replace(openRow + 1, closeRow - 1, std::move(out));

  return {};
}

// --- Pass (b): definitions ---

// The engine's hooks a game class overrides, for a class no header declares.
constexpr std::array<std::string_view, 19> KNOWN_HOOKS = {"create",
                                                          "update",
                                                          "lateUpdate",
                                                          "render",
                                                          "onEntityAdded",
                                                          "onEntityRemoved",
                                                          "settings",
                                                          "onCreate",
                                                          "onDestroy",
                                                          "onUpdate",
                                                          "onProcessInput",
                                                          "onCollisionEnter",
                                                          "onCollisionExit",
                                                          "onTriggerEnter",
                                                          "onTriggerExit",
                                                          "onCollisionEnter2D",
                                                          "onCollisionExit2D",
                                                          "onTriggerEnter2D",
                                                          "onTriggerExit2D"};

bool isKnownHook(std::string_view name)
{
  return std::find(KNOWN_HOOKS.begin(), KNOWN_HOOKS.end(), name) !=
         KNOWN_HOOKS.end();
}

// A class a header (or the file) declares.
struct KnownClass
{
  std::vector<std::string> path; // its name with the classes around it
  std::set<std::string> overrides;
};

// What the file's headers, and the file, declare.
struct Known
{
  std::vector<KnownClass> classes; // in the order they're declared
  std::set<std::string> namespaces;
  std::set<std::string> functions; // declared outside classes, not defined
};

enum class Role : uint8_t
{
  Barrier,
  Head,
  Member,
  Free,
};

// One entry of a scope body.
struct Definition
{
  const Entry* entry = nullptr;
  Role role = Role::Barrier;
  std::string name;
  int classRank = 0;
  int category = 4; // 0 ctor, 1 dtor, 2 assignment, 3 override, 4 other
  int hookRank = 4;
  bool risky = false; // a deduced return type or constexpr: order matters
  uint32_t index = 0;
  std::unordered_set<std::string_view> uses;
  std::vector<std::string> functions; // free functions a head entry declares
};

class DefinitionPass
{
public:
  DefinitionPass(std::string_view text,
                 const LineIndex& lines,
                 Rewrite& rewrite,
                 const Known& known)
      : m_text(text), m_lines(lines), m_rewrite(rewrite), m_known(known)
  {
  }

  // Orders one scope body (between `openRow` and `closeRow`); empty, or why
  // the file keeps its order.
  std::string order(TSNode body, int64_t openRow, int64_t closeRow);

private:
  std::string classifyFunction(TSNode function, Definition& definition);
  std::string orderSegment(std::vector<Definition>& segment);

  std::string_view m_text;
  const LineIndex& m_lines;
  Rewrite& m_rewrite;
  const Known& m_known;
  // Scopes no header declares, taken as classes for their constructors,
  // destructors or engine hooks, in order of first definition.
  std::vector<std::string> m_guessed;
  std::set<std::string> m_classScopes; // every class scope the file defines
};

std::string DefinitionPass::classifyFunction(TSNode node,
                                             Definition& definition)
{
  const std::vector<TSNode> found = declarators(node);
  const TSNode function =
      found.empty() ? TSNode{} : functionDeclarator(found.front());

  if (ts_node_is_null(function) || isType(function, "operator_cast"))
  {
    definition.role = Role::Barrier;
    return {};
  }

  definition.name = functionName(function, m_text);
  definition.risky =
      hasWord(node, m_text, {"constexpr", "consteval"}) ||
      (isType(field(node, "type"), "placeholder_type_specifier") &&
       [&]
       {
         const uint32_t count = ts_node_named_child_count(function);

         for (uint32_t i = 0; i < count; ++i)
           if (isType(ts_node_named_child(function, i), "trailing_return_type"))
             return false;

         return true;
       }());
  const std::vector<std::string> scope = scopeOf(function, m_text);

  if (scope.empty())
  {
    definition.role = Role::Free;
    return {};
  }

  // The class whose name ends the scope, longest match first.
  int best = -1;
  std::size_t bestLength = 0;

  for (std::size_t c = 0; c < m_known.classes.size(); ++c)
  {
    const std::vector<std::string>& path = m_known.classes[c].path;

    if (path.size() > scope.size() || path.size() <= bestLength)
      continue;

    if (std::equal(path.rbegin(), path.rend(), scope.rbegin()))
    {
      best = static_cast<int>(c);
      bestLength = path.size();
    }
  }

  std::string key;

  for (const std::string& part : scope)
    key += (key.empty() ? "" : "::") + part;

  const bool isConstructor = definition.name == scope.back();
  const bool isDestructor = definition.name == "~" + scope.back();

  if (best < 0)
  {
    if (m_known.namespaces.count(scope.back()))
    {
      definition.role = Role::Free;
      return {};
    }

    if (!m_classScopes.count(key))
      return "it can't tell whether " + key +
             " is a class or a namespace (no header it can find declares it)";

    auto at = std::find(m_guessed.begin(), m_guessed.end(), key);

    if (at == m_guessed.end())
      at = m_guessed.insert(m_guessed.end(), key);

    definition.classRank = static_cast<int>(m_known.classes.size()) +
                           static_cast<int>(at - m_guessed.begin());
  }
  else
    definition.classRank = best;

  definition.role = Role::Member;
  const bool overrides =
      best >= 0
          ? m_known.classes[static_cast<std::size_t>(best)].overrides.count(
                definition.name) > 0
          : isKnownHook(definition.name);

  if (isConstructor)
    definition.category = 0;
  else if (isDestructor)
    definition.category = 1;
  else if (definition.name == "operator=")
    definition.category = 2;
  else if (overrides)
  {
    definition.category = 3;
    static constexpr std::array<std::string_view, 4> FIRST_HOOKS = {
        "create", "onCreate", "update", "onUpdate"};
    const auto at =
        std::find(FIRST_HOOKS.begin(), FIRST_HOOKS.end(), definition.name);
    definition.hookRank = static_cast<int>(at - FIRST_HOOKS.begin());
  }

  return {};
}

std::string
DefinitionPass::order(TSNode body, int64_t openRow, int64_t closeRow)
{
  std::vector<Entry> entries;

  if (std::string why = splitEntries(body, m_lines, openRow, closeRow, entries);
      !why.empty())
    return why;

  // Scopes the file defines constructors, destructors or hooks of are
  // classes, when no header says what they are.
  for (const Entry& entry : entries)
  {
    if (!hasNode(entry))
      continue;

    TSNode node = entry.node;

    if (isType(node, "template_declaration"))
      node = ts_node_named_child(node, ts_node_named_child_count(node) - 1);

    if (!isType(node, "function_definition"))
      continue;

    const std::vector<TSNode> found = declarators(node);
    const TSNode function =
        found.empty() ? TSNode{} : functionDeclarator(found.front());

    if (ts_node_is_null(function) || isType(function, "operator_cast"))
      continue;

    const std::vector<std::string> scope = scopeOf(function, m_text);

    if (scope.empty())
      continue;

    const std::string name = functionName(function, m_text);

    if (name == scope.back() || name == "~" + scope.back() || isKnownHook(name))
    {
      std::string key;

      for (const std::string& part : scope)
        key += (key.empty() ? "" : "::") + part;

      m_classScopes.insert(key);
    }
  }

  std::vector<Definition> segment;

  for (const Entry& entry : entries)
  {
    Definition definition;
    definition.entry = &entry;
    definition.index = static_cast<uint32_t>(segment.size());
    const TSNode node = entry.node;
    const std::string_view type =
        hasNode(entry) ? ts_node_type(node) : std::string_view();

    if (type == "preproc_if" || type == "preproc_ifdef")
      return "it has an #if or #ifdef region: " +
             quoted(m_lines, entry.nodeFirst);

    if (hasNode(entry) && (type == "ERROR" || parsesBadly(node, false)))
      return "tree-sitter misreads " + quoted(m_lines, entry.nodeFirst);

    if (type == "function_definition")
    {
      if (std::string why = classifyFunction(node, definition); !why.empty())
        return why;
    }
    else if (type == "template_declaration")
    {
      const TSNode parameters = field(node, "parameters");
      const TSNode inner =
          ts_node_named_child(node, ts_node_named_child_count(node) - 1);

      if (ts_node_named_child_count(parameters) == 0)
        definition.role = Role::Barrier;
      else if (isType(inner, "function_definition"))
      {
        if (std::string why = classifyFunction(inner, definition); !why.empty())
          return why;
      }
      else
        definition.role = Role::Head;
    }
    else if (type == "declaration" || type == "class_specifier" ||
             type == "struct_specifier" || type == "union_specifier" ||
             type == "enum_specifier" || type == "alias_declaration" ||
             type == "type_definition" || type == "static_assert_declaration" ||
             type == "concept_definition")
    {
      definition.role = Role::Head;

      if (type == "declaration")
        for (const TSNode declarator : declarators(node))
          if (const TSNode function = functionDeclarator(declarator);
              !ts_node_is_null(function) && scopeOf(function, m_text).empty())
            definition.functions.push_back(functionName(function, m_text));
    }
    else
      definition.role = Role::Barrier;

    if (definition.role == Role::Barrier)
    {
      if (std::string why = orderSegment(segment); !why.empty())
        return why;

      segment.clear();
      continue;
    }

    collectUses(node, m_text, false, false, definition.uses);
    segment.push_back(std::move(definition));
  }

  return orderSegment(segment);
}

std::string DefinitionPass::orderSegment(std::vector<Definition>& segment)
{
  if (segment.size() < 2)
    return {};

  for (uint32_t i = 0; i < segment.size(); ++i)
    segment[i].index = i;

  std::vector<bool> pinned(segment.size(), false);
  std::vector<uint32_t> order;
  std::vector<uint32_t> position(segment.size());
  const auto arrange = [&]
  {
    order.clear();

    for (const Definition& d : segment)
      if (d.role == Role::Head || (d.role == Role::Free && pinned[d.index]))
        order.push_back(d.index);

    const std::size_t members = order.size();

    for (const Definition& d : segment)
      if (d.role == Role::Member)
        order.push_back(d.index);

    std::stable_sort(order.begin() + static_cast<std::ptrdiff_t>(members),
                     order.end(),
                     [&](uint32_t a, uint32_t b)
                     {
                       const Definition& x = segment[a];
                       const Definition& y = segment[b];

                       if (x.classRank != y.classRank)
                         return x.classRank < y.classRank;

                       if (x.category != y.category)
                         return x.category < y.category;

                       if (x.category < 3)
                         return false;

                       if (x.category == 3 && x.hookRank != y.hookRank)
                         return x.hookRank < y.hookRank;

                       return sortKey(x.name) < sortKey(y.name);
                     });

    for (const Definition& d : segment)
      if (d.role == Role::Free && !pinned[d.index])
        order.push_back(d.index);

    for (uint32_t at = 0; at < order.size(); ++at)
      position[order[at]] = at;
  };
  // Something that moves above a definition and uses its name.
  const auto usedAbove = [&](const Definition& target)
  {
    for (const Definition& user : segment)
      if (user.index > target.index &&
          position[user.index] < position[target.index] &&
          user.uses.count(target.name))
        return &user;

    return static_cast<const Definition*>(nullptr);
  };

  // A helper that something moving above it uses stays with the head, unless
  // it's declared before.
  for (bool changed = true; changed;)
  {
    changed = false;
    arrange();

    for (const Definition& d : segment)
      if (d.role == Role::Free && !pinned[d.index] &&
          !m_known.functions.count(d.name) && usedAbove(d))
      {
        pinned[d.index] = true;
        changed = true;
      }
  }

  for (const Definition& d : segment)
    if (d.risky && (d.role == Role::Member || d.role == Role::Free))
      if (const Definition* user = usedAbove(d))
        return quoted(m_lines, user->entry->nodeFirst) + " uses " + d.name +
               ", whose deduced return type or constexpr needs its "
               "definition first";

  // A free function declared or defined above a definition that came before
  // it would add to what that definition's calls of its name can see.
  for (const Definition& d : segment)
  {
    std::vector<std::string_view> declared(
        d.functions.begin(), d.functions.end());

    if (d.role == Role::Free)
      declared.push_back(d.name);

    for (const Definition& user : segment)
      if (user.index < d.index && position[user.index] > position[d.index])
        for (const std::string_view name : declared)
          if (user.uses.count(name))
            return quoted(m_lines, user.entry->nodeFirst) + " would see the " +
                   std::string(name) +
                   " declared after it, which may change what its call means";
  }

  bool moved = false;

  for (uint32_t at = 0; at < order.size(); ++at)
    moved |= order[at] != at;

  if (!moved)
    return {};

  const std::string lineBreak(format::lineBreakOf(m_text));
  std::string out;

  for (uint32_t at = 0; at < order.size(); ++at)
  {
    const Definition& d = segment[order[at]];

    if (at > 0)
    {
      const Definition& previous = segment[order[at - 1]];

      if (previous.index + 1 == d.index)
        m_rewrite.render(previous.entry->last + 1, d.entry->first - 1, out);
      else
        out += lineBreak;
    }

    m_rewrite.render(d.entry->first, d.entry->last, out);
  }

  m_rewrite.replace(
      segment.front().entry->first, segment.back().entry->last, std::move(out));
  return {};
}

std::vector<std::string> splitScope(std::string_view spelled)
{
  std::vector<std::string> parts;

  for (std::size_t start = 0; start <= spelled.size();)
  {
    std::size_t at = spelled.find("::", start);

    if (at == std::string_view::npos)
      at = spelled.size();

    if (const std::string_view part =
            lint::trimmed(spelled.substr(start, at - start));
        !part.empty())
      parts.emplace_back(part);

    start = at + 2;
  }

  return parts;
}

void collectDeclared(TSNode node,
                     std::string_view text,
                     std::vector<std::string>& path,
                     Known& known)
{
  const uint32_t count = ts_node_named_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_named_child(node, i);
    const std::string_view type = ts_node_type(child);

    if (type == "compound_statement")
      continue;

    if (type == "namespace_definition")
    {
      for (std::string& part : splitScope(nodeText(field(child, "name"), text)))
        known.namespaces.insert(std::move(part));

      collectDeclared(child, text, path, known);
      continue;
    }

    if ((type == "class_specifier" || type == "struct_specifier") &&
        !ts_node_is_null(field(child, "body")) &&
        !ts_node_is_null(field(child, "name")))
    {
      const std::size_t depth = path.size();

      for (std::string& part : splitScope(nodeText(field(child, "name"), text)))
        path.push_back(std::move(part));

      KnownClass made{path, {}};
      const TSNode body = field(child, "body");
      const uint32_t members = ts_node_named_child_count(body);

      for (uint32_t m = 0; m < members; ++m)
      {
        TSNode member = ts_node_named_child(body, m);

        if (isType(member, "template_declaration"))
          member = ts_node_named_child(
              member, ts_node_named_child_count(member) - 1);

        const std::vector<TSNode> found = declarators(member);

        if (found.empty())
          continue;

        const TSNode function = functionDeclarator(found.front());

        if (ts_node_is_null(function))
          continue;

        const uint32_t parts = ts_node_named_child_count(function);

        for (uint32_t p = 0; p < parts; ++p)
          if (isType(ts_node_named_child(function, p), "virtual_specifier"))
            made.overrides.insert(functionName(function, text));
      }

      known.classes.push_back(std::move(made));
      collectDeclared(body, text, path, known);
      path.resize(depth);
      continue;
    }

    if (type == "declaration" && path.empty())
    {
      for (const TSNode declarator : declarators(child))
        if (const TSNode function = functionDeclarator(declarator);
            !ts_node_is_null(function))
          known.functions.insert(functionName(function, text));

      continue;
    }

    if (type == "function_definition" || type == "field_declaration_list")
      continue;

    collectDeclared(child, text, path, known);
  }
}

// The headers a source's classes are declared in: the one beside it, then its
// quoted includes, as a compiler would find them from the folders above it.
std::vector<std::filesystem::path>
headersOf(const std::filesystem::path& file, TSNode root, std::string_view text)
{
  constexpr std::size_t MOST_HEADERS = 16;
  std::vector<std::filesystem::path> found;
  std::error_code error;
  const auto add = [&](const std::filesystem::path& candidate)
  {
    const std::filesystem::path normal = candidate.lexically_normal();

    if (std::find(found.begin(), found.end(), normal) != found.end() ||
        !std::filesystem::is_regular_file(normal, error))
      return false;

    found.push_back(normal);
    return true;
  };

  for (const char* extension : {".h", ".hpp", ".hh", ".hxx"})
    if (add(std::filesystem::path(file).replace_extension(extension)))
      break;

  const uint32_t count = ts_node_named_child_count(root);

  for (uint32_t i = 0; i < count && found.size() < MOST_HEADERS; ++i)
  {
    const TSNode child = ts_node_named_child(root, i);

    if (!isType(child, "preproc_include"))
      continue;

    const TSNode path = field(child, "path");

    if (!isType(path, "string_literal"))
      continue;

    std::string_view spelled = nodeText(path, text);

    if (spelled.size() < 2)
      continue;

    spelled = spelled.substr(1, spelled.size() - 2);
    std::filesystem::path folder = file.parent_path();

    if (add(folder / spelled))
      continue;

    for (int depth = 0; depth < 8 && folder.has_parent_path() &&
                        folder != folder.parent_path();
         ++depth)
    {
      folder = folder.parent_path();

      if (add(folder / spelled) || add(folder / "include" / spelled))
        break;
    }
  }

  return found;
}

// Every body of a namespace in `node`'s scope bodies, innermost last.
void namespaceBodies(TSNode node, std::vector<TSNode>& out)
{
  const uint32_t count = ts_node_named_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_named_child(node, i);

    if (!isType(child, "namespace_definition"))
      continue;

    const TSNode body = field(child, "body");

    if (ts_node_is_null(body))
      continue;

    out.push_back(body);
    namespaceBodies(body, out);
  }
}

// Every class and struct body in the tree, outer before inner.
void classBodies(TSNode node, std::vector<std::pair<TSNode, TSNode>>& out)
{
  const uint32_t count = ts_node_named_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_named_child(node, i);
    const std::string_view type = ts_node_type(child);

    if (type == "compound_statement")
      continue;

    if (type == "class_specifier" || type == "struct_specifier")
      if (const TSNode body = field(child, "body"); !ts_node_is_null(body))
        out.emplace_back(child, body);

    classBodies(child, out);
  }
}

} // namespace

std::string orderClassBodies(std::string_view text,
                             const TSTree* tree,
                             bool sortFields,
                             std::vector<std::string>& notes)
{
  const LineIndex lines(text);
  Rewrite rewrite(lines);
  std::vector<std::pair<TSNode, TSNode>> bodies;
  classBodies(ts_tree_root_node(tree), bodies);
  ClassPass pass(text, lines, rewrite, sortFields, notes);

  for (auto it = bodies.rbegin(); it != bodies.rend(); ++it)
    pass.order(it->first, it->second);

  if (!rewrite.changed())
    return std::string(text);

  std::string out;
  out.reserve(text.size() + 256);
  rewrite.render(0, lines.count() - 1, out);
  return out;
}

std::string orderDefinitions(std::string_view text,
                             const TSTree* tree,
                             const std::filesystem::path& path,
                             TSParser* parser,
                             const FileReader& read,
                             std::vector<std::string>& notes)
{
  const TSNode root = ts_tree_root_node(tree);

  Known known;
  std::vector<std::string> scope;

  for (const std::filesystem::path& header : headersOf(path, root, text))
  {
    const std::string source = read(header);

    if (source.empty())
      continue;

    TSTree* parsed = ts_parser_parse_string(
        parser, nullptr, source.data(), static_cast<uint32_t>(source.size()));

    if (!parsed)
      continue;

    collectDeclared(ts_tree_root_node(parsed), source, scope, known);
    ts_tree_delete(parsed);
  }

  collectDeclared(root, text, scope, known);

  const LineIndex lines(text);
  Rewrite rewrite(lines);
  DefinitionPass pass(text, lines, rewrite, known);
  std::vector<TSNode> bodies;
  namespaceBodies(root, bodies);

  for (auto it = bodies.rbegin(); it != bodies.rend(); ++it)
  {
    const TSNode body = *it;

    if (std::string why =
            pass.order(body, firstRow(body), ts_node_end_point(body).row);
        !why.empty())
    {
      notes.push_back("the definitions keep their order: " + why);
      return std::string(text);
    }
  }

  if (std::string why =
          pass.order(root, -1, static_cast<int64_t>(lines.count()));
      !why.empty())
  {
    notes.push_back("the definitions keep their order: " + why);
    return std::string(text);
  }

  if (!rewrite.changed())
    return std::string(text);

  std::string out;
  out.reserve(text.size());
  rewrite.render(0, lines.count() - 1, out);
  return out;
}

} // namespace sfs::lsp
