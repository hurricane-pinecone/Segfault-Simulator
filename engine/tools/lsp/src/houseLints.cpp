#include "houseLints.h"

#include "document.h"
#include "lintSupport.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/parser.h"
#include "sfs/codegen/rules.h"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs::lsp
{

using codegen::Diagnostic;
using codegen::RuleId;
using codegen::SourceRange;
using codegen::TextEdit;
using lint::childCountOf;
using lint::end;
using lint::isType;
using lint::namedChild;
using lint::namedCount;
using lint::Next;
using lint::nthChild;
using lint::parent;
using lint::same;
using lint::start;
using lint::typeName;

namespace
{

// The node kinds the house walk tells apart.
enum class HouseKind : uint8_t
{
  Other,
  Conditional,
  Lambda,
  Declaration,
  If,
};

// Where a constexpr variable is declared, which decides whether renaming it
// can be fixed in its file alone.
enum class ConstantScope : uint8_t
{
  Block,
  Namespace,
  Class,
};

// A constexpr variable not named in UPPER_SNAKE_CASE.
struct Constant
{
  TSNode declaration;
  TSNode name;
  std::string_view written;
  std::string fresh;
};

bool isLower(char c) { return c >= 'a' && c <= 'z'; }

bool isUpperSnake(std::string_view name)
{
  return std::none_of(name.begin(), name.end(), isLower);
}

bool isBlank(std::string_view text)
{
  return std::all_of(
      text.begin(), text.end(), [](char c) { return lint::isSpace(c); });
}

// Whether a node holds no code: a comment, or a literal's text.
bool isProse(TSNode node)
{
  static constexpr const char* PROSE[] = {"comment",
                                          "string_literal",
                                          "string_content",
                                          "raw_string_literal",
                                          "raw_string_content",
                                          "raw_string_delimiter",
                                          "char_literal",
                                          "character",
                                          "escape_sequence",
                                          "system_lib_string"};
  const char* type = typeName(node);
  return std::any_of(std::begin(PROSE),
                     std::end(PROSE),
                     [&](const char* prose)
                     { return std::strcmp(type, prose) == 0; });
}

// Whether `node` is its parent's child under the field `field`.
bool isFieldChild(TSNode node, const char* field)
{
  const TSNode owner = parent(node);
  const uint32_t count = childCountOf(owner);

  for (uint32_t i = 0; i < count; ++i)
    if (same(nthChild(owner, i), node))
    {
      const char* name = ts_node_field_name_for_child(owner, i);
      return name && std::strcmp(name, field) == 0;
    }

  return false;
}

// Whether an identifier is the name a declaration declares, not a use.
bool isDeclaredName(TSNode identifier)
{
  const TSNode owner = parent(identifier);

  if (isType(owner, "reference_declarator") ||
      isType(owner, "structured_binding_declarator") ||
      isType(owner, "variadic_declarator"))
    return true;

  if (isType(owner, "lambda_capture_initializer"))
    return same(namedChild(owner, 0), identifier);

  if (isType(owner, "enumerator"))
    return isFieldChild(identifier, "name");

  return isFieldChild(identifier, "declarator");
}

// Whether a type name may be a value the grammar can't tell from a type: a
// template argument on its own (`f<kSize>()`), or the argument of a block's
// object made with parentheses, which it reads as a function's declaration
// (`std::string text(kPrefix);`).
bool isValueAsType(TSNode node)
{
  if (!isType(node, "type_identifier"))
    return false;

  const TSNode holder = parent(node);

  if (isType(holder, "type_descriptor"))
    return namedCount(holder) == 1 &&
           isType(parent(holder), "template_argument_list");

  const TSNode function = parent(parent(holder));
  return isType(holder, "parameter_declaration") && namedCount(holder) == 1 &&
         isType(function, "function_declarator") &&
         isType(parent(function), "declaration") &&
         isType(parent(parent(function)), "compound_statement");
}

// The variable a declarator declares, through init, pointer, reference and
// array declarators; null for a function or a qualified name.
TSNode variableName(TSNode declarator)
{
  while (!ts_node_is_null(declarator))
  {
    if (isType(declarator, "identifier") ||
        isType(declarator, "field_identifier"))
      return declarator;

    if (isType(declarator, "init_declarator") ||
        isType(declarator, "pointer_declarator") ||
        isType(declarator, "array_declarator"))
      declarator = lint::LintFile::field(declarator, "declarator");
    else if (isType(declarator, "reference_declarator"))
      declarator = namedChild(declarator, namedCount(declarator) - 1);
    else
      return {};
  }

  return {};
}

// The start of the line holding byte `at`.
uint32_t lineStartOf(std::string_view source, uint32_t at)
{
  if (at == 0)
    return 0;

  const std::size_t lineBreak = source.rfind('\n', at - 1);
  return lineBreak == std::string_view::npos
             ? 0
             : static_cast<uint32_t>(lineBreak + 1);
}

// The line break the file uses at byte `at`'s line.
std::string_view lineBreakAt(std::string_view source, uint32_t at)
{
  const std::size_t lineBreak = source.find('\n', at);
  return lineBreak != std::string_view::npos && lineBreak > 0 &&
                 source[lineBreak - 1] == '\r'
             ? "\r\n"
             : "\n";
}

// `block`'s lines moved from the first one's indent to `indent`, joined by
// `lineBreak`. Lines indented less keep what they have (a preprocessor line),
// less what the move takes off. False when the first line's indent doesn't
// start with `indent`.
bool reindented(std::string_view block,
                std::string_view indent,
                std::string_view lineBreak,
                std::string& out)
{
  std::vector<std::string_view> lines;

  for (std::size_t from = 0; from <= block.size();)
  {
    std::size_t to = block.find('\n', from);

    if (to == std::string_view::npos)
      to = block.size();

    std::string_view line = block.substr(from, to - from);

    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);

    lines.push_back(line);
    from = to + 1;
  }

  std::string_view base;

  for (const std::string_view line : lines)
    if (!isBlank(line))
    {
      base = line.substr(0, line.find_first_not_of(" \t"));
      break;
    }

  if (!base.starts_with(indent))
    return false;

  const std::size_t extra = base.size() - indent.size();

  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    const std::string_view line = lines[i];

    if (i > 0)
      out += lineBreak;

    if (isBlank(line))
      continue;

    if (line.starts_with(base))
    {
      out += indent;
      out += line.substr(base.size());
      continue;
    }

    const std::size_t leading = line.find_first_not_of(" \t");
    out += line.substr(std::min(leading, extra));
  }

  return true;
}

// The statement an else clause runs (comments aside), or null.
TSNode elseBody(TSNode elseClause)
{
  for (uint32_t i = namedCount(elseClause); i > 0; --i)
  {
    const TSNode child = namedChild(elseClause, i - 1);

    if (!isType(child, "comment"))
      return child;
  }

  return {};
}

// The jump a statement always ends in ("return", "continue", "break" or
// "throw"), or empty: the statement itself, a block's last statement, or one
// that every branch of an if/else chain ends in.
std::string_view endingJump(TSNode statement)
{
  if (isType(statement, "return_statement"))
    return "return";

  if (isType(statement, "continue_statement"))
    return "continue";

  if (isType(statement, "break_statement"))
    return "break";

  if (isType(statement, "throw_statement"))
    return "throw";

  if (isType(statement, "compound_statement"))
  {
    for (uint32_t i = namedCount(statement); i > 0; --i)
    {
      const TSNode last = namedChild(statement, i - 1);

      if (!isType(last, "comment"))
        return endingJump(last);
    }

    return {};
  }

  if (!isType(statement, "if_statement"))
    return {};

  const TSNode alternative = lint::LintFile::field(statement, "alternative");

  if (ts_node_is_null(alternative))
    return {};

  const std::string_view consequence =
      endingJump(lint::LintFile::field(statement, "consequence"));
  const std::string_view otherwise = endingJump(elseBody(alternative));
  return otherwise.empty() ? std::string_view{} : consequence;
}

// One run of the house lints over a file.
struct HouseRun
{
  std::string_view source;
  const std::filesystem::path* path = nullptr;
  std::vector<Diagnostic>* out = nullptr;
  TSNode root{};
  const std::vector<HouseKind>* kinds = nullptr;
  std::vector<Constant> constants;
  std::vector<std::string_view> names;
  std::vector<std::string> namespaces;

  HouseKind kind(TSNode node) const
  {
    const TSSymbol symbol = ts_node_symbol(node);
    return symbol < kinds->size() ? (*kinds)[symbol] : HouseKind::Other;
  }

  std::string_view text(TSNode node) const
  {
    return codegen::nodeText(source, node);
  }

  void report(RuleId rule,
              TSNode node,
              std::string message,
              std::string fixTitle = {},
              std::vector<TextEdit> fix = {})
  {
    out->push_back({rule,
                    *path,
                    codegen::rangeOf(node),
                    std::move(message),
                    std::move(fixTitle),
                    std::move(fix)});
  }

  // Calls `visit` with each place `word` is written as a whole word, in code
  // (not a comment or a literal), in source[from, to); stops when it returns
  // false, and then returns false.
  template <typename Visit>
  bool forEachCodeWord(std::string_view word,
                       uint32_t from,
                       uint32_t to,
                       Visit&& visit) const
  {
    if (word.empty())
      return true;

    for (std::size_t at = source.find(word, from);
         at != std::string_view::npos && at + word.size() <= to;
         at = source.find(word, at + 1))
    {
      const std::size_t after = at + word.size();

      if ((at > 0 && isIdentifierChar(source[at - 1])) ||
          (after < source.size() && isIdentifierChar(source[after])))
        continue;

      const TSNode node = ts_node_descendant_for_byte_range(
          root, static_cast<uint32_t>(at), static_cast<uint32_t>(after));

      if (isProse(node))
        continue;

      if (!visit(node, static_cast<uint32_t>(at)))
        return false;
    }

    return true;
  }

  // Whether `word` is written in code in source[from, to).
  bool usesWord(std::string_view word, uint32_t from, uint32_t to) const
  {
    return !forEachCodeWord(
        word, from, to, [](TSNode, uint32_t) -> bool { return false; });
  }

  void checkTernary(TSNode conditional)
  {
    for (TSNode at = parent(conditional); !ts_node_is_null(at); at = parent(at))
    {
      const HouseKind atKind = kind(at);

      if (atKind == HouseKind::Lambda || atKind == HouseKind::Declaration)
        return;

      if (atKind == HouseKind::Conditional)
      {
        report(RuleId::NestedTernary,
               conditional,
               "Ternaries should not be nested: two are fine kept apart, but "
               "not one inside another. Work the inner one out into its own "
               "named variable first, and use that in the outer ternary.");
        return;
      }

      const std::string_view type = typeName(at);

      if (type.ends_with("_statement") || type == "function_definition" ||
          type == "translation_unit")
        return;
    }
  }

  void collectConstants(TSNode declaration)
  {
    bool constant = false;
    const uint32_t count = childCountOf(declaration);

    for (uint32_t i = 0; i < count && !constant; ++i)
    {
      const TSNode child = nthChild(declaration, i);
      constant = isType(child, "type_qualifier") && text(child) == "constexpr";
    }

    if (!constant)
      return;

    for (uint32_t i = 0; i < count; ++i)
    {
      const char* field = ts_node_field_name_for_child(declaration, i);

      if (!field || std::strcmp(field, "declarator") != 0)
        continue;

      const TSNode name = variableName(nthChild(declaration, i));

      if (ts_node_is_null(name) || isUpperSnake(text(name)))
        continue;

      const std::string_view written = text(name);
      constants.push_back(
          {declaration, name, written, upperSnakeName(written)});
    }
  }

  // Where a constant is declared; `within` is set to the node of its scope:
  // for a block, the statement or block its name is seen in. An `#if` branch
  // isn't a scope: its declarations are its enclosing scope's.
  static ConstantScope scopeOf(TSNode declaration, TSNode& within)
  {
    for (TSNode at = parent(declaration); !ts_node_is_null(at); at = parent(at))
    {
      if (isType(at, "template_declaration") ||
          isType(at, "condition_clause") || isType(at, "init_statement") ||
          std::string_view(ts_node_type(at)).starts_with("preproc_"))
        continue;

      within = at;

      if (isType(at, "field_declaration_list") ||
          isType(at, "linkage_specification"))
        return ConstantScope::Class;

      if (isType(at, "declaration_list") || isType(at, "translation_unit"))
        return ConstantScope::Namespace;

      if (isType(at, "case_statement"))
        within = parent(at);

      return ConstantScope::Block;
    }

    within = TSNode{};
    return ConstantScope::Namespace;
  }

  // Whether `name` declares the same constant again in another `#if` branch:
  // a constant of the same name in the same scope.
  bool isBranchTwin(const Constant& constant, TSNode name)
  {
    TSNode within{};
    const ConstantScope scope = scopeOf(constant.declaration, within);

    return std::any_of(
        constants.begin(),
        constants.end(),
        [&](const Constant& other)
        {
          if (!same(other.name, name) || other.written != constant.written)
            return false;

          TSNode otherWithin{};
          return scopeOf(other.declaration, otherWithin) == scope &&
                 same(otherWithin, within);
        });
  }

  // The edits renaming a constant where it's seen; false when a use can't be
  // told from another name or renamed safely: one the scope declares again,
  // one in a macro, a qualified one that may be the constant's, or for a
  // namespace constant, one above it. Members (`p.kMax`) and names qualified
  // by another scope aren't the constant's, and are left as they are. Its
  // declarations in other `#if` branches are renamed with it, and it's seen
  // from the first of them.
  bool renameEdits(const Constant& constant,
                   ConstantScope scope,
                   TSNode block,
                   std::vector<TextEdit>& edits)
  {
    uint32_t from = start(constant.name);

    for (const Constant& other : constants)
      if (isBranchTwin(constant, other.name))
        from = std::min(from, start(other.name));

    const bool inBlock = scope == ConstantScope::Block;
    const uint32_t to =
        inBlock ? end(block) : static_cast<uint32_t>(source.size());

    if (!inBlock)
      lint::namespacesOf(constant.declaration, source, namespaces);

    return forEachCodeWord(
        constant.written,
        inBlock ? from : 0,
        to,
        [&](TSNode node, uint32_t at)
        {
          if (same(node, constant.name) || isBranchTwin(constant, node))
          {
            edits.push_back({codegen::rangeOf(node), constant.fresh});
            return true;
          }

          if (at < from || start(node) != at ||
              end(node) != at + constant.written.size())
            return false;

          const TSNode owner = parent(node);

          if (isType(node, "field_identifier"))
            return isType(owner, "field_expression") ||
                   isType(owner, "field_designator");

          if (isType(owner, "qualified_identifier"))
          {
            const TSNode qualifier = lint::LintFile::field(owner, "scope");

            if (ts_node_is_null(qualifier))
              return false;

            const std::string_view named = lint::lastName(text(qualifier));
            return inBlock || std::none_of(namespaces.begin(),
                                           namespaces.end(),
                                           [&](const std::string& space)
                                           { return space == named; });
          }

          const bool use = isType(node, "identifier") ? !isDeclaredName(node)
                                                      : isValueAsType(node);

          if (use)
            edits.push_back({codegen::rangeOf(node), constant.fresh});

          return use;
        });
  }

  void checkConstants()
  {
    const bool header = isHeader(*path);

    for (const Constant& constant : constants)
    {
      const bool clash =
          std::any_of(constants.begin(),
                      constants.end(),
                      [&](const Constant& other) {
                        return other.written != constant.written &&
                               other.fresh == constant.fresh;
                      });
      const bool taken =
          clash ||
          usesWord(constant.fresh, 0, static_cast<uint32_t>(source.size()));
      TSNode block{};
      const ConstantScope scope = scopeOf(constant.declaration, block);
      bool external = false;
      const uint32_t count = childCountOf(constant.declaration);

      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode child = nthChild(constant.declaration, i);
        external |=
            isType(child, "storage_class_specifier") && text(child) == "extern";
      }

      const bool local =
          scope == ConstantScope::Block ||
          (scope == ConstantScope::Namespace && !header && !external);

      std::string message = "A `constexpr` is named in UPPER_SNAKE_CASE: `" +
                            std::string(constant.written) + "` would be `" +
                            constant.fresh + "`.";
      std::vector<TextEdit> edits;

      if (taken)
      {
        message += " That name is already used in this file, so choose one "
                   "by hand.";
        report(RuleId::ConstexprName, constant.name, std::move(message));
        continue;
      }

      if (local && renameEdits(constant, scope, block, edits))
      {
        report(RuleId::ConstexprName,
               constant.name,
               std::move(message),
               "Rename to " + constant.fresh,
               std::move(edits));
        continue;
      }

      message += " Rename it with clangd, which finds every use.";
      report(RuleId::ConstexprName, constant.name, std::move(message));
    }
  }

  // Adds the names a statement declares in its block: variables, functions,
  // types and aliases.
  void addDeclared(TSNode statement, std::vector<std::string_view>& found)
  {
    const TSNode type = lint::LintFile::field(statement, "type");

    if (!ts_node_is_null(lint::LintFile::field(type, "body")))
    {
      const TSNode typeName = lint::LintFile::field(type, "name");

      if (!ts_node_is_null(typeName))
        found.push_back(text(typeName));
    }

    if (isType(statement, "class_specifier") ||
        isType(statement, "struct_specifier") ||
        isType(statement, "union_specifier") ||
        isType(statement, "enum_specifier") ||
        isType(statement, "alias_declaration") ||
        isType(statement, "namespace_alias_definition"))
    {
      const TSNode name = lint::LintFile::field(statement, "name");

      if (!ts_node_is_null(name))
        found.push_back(text(name));

      return;
    }

    if (isType(statement, "using_declaration"))
    {
      found.push_back(lint::lastName(text(namedChild(statement, 0))));
      return;
    }

    if (!isType(statement, "declaration") &&
        !isType(statement, "type_definition"))
      return;

    const uint32_t count = childCountOf(statement);

    for (uint32_t i = 0; i < count; ++i)
    {
      const char* field = ts_node_field_name_for_child(statement, i);

      if (!field || std::strcmp(field, "declarator") != 0)
        continue;

      lint::walkTree(
          nthChild(statement, i),
          [&](TSNode node)
          {
            if (isType(node, "identifier") || isType(node, "type_identifier"))
            {
              if (isDeclaredName(node))
                found.push_back(text(node));

              return Next::Skip;
            }

            // A declarator's value or parameters aren't the
            // statement's names.
            const bool declarator =
                isType(node, "init_declarator") ||
                isType(node, "pointer_declarator") ||
                isType(node, "reference_declarator") ||
                isType(node, "array_declarator") ||
                isType(node, "function_declarator") ||
                isType(node, "structured_binding_declarator");
            return declarator ? Next::Descend : Next::Skip;
          });
    }
  }

  bool named(std::string_view name) const
  {
    return std::find(names.begin(), names.end(), name) != names.end();
  }

  // The fix of an else after a jump, in `edits`; false when it isn't safe or
  // the code's layout can't be kept.
  bool elseFix(TSNode ifNode,
               TSNode consequence,
               TSNode elseClause,
               TSNode body,
               std::vector<TextEdit>& edits)
  {
    if (ts_node_has_error(ifNode))
      return false;

    const uint32_t lineStart = lineStartOf(source, start(ifNode));
    const std::string_view indent =
        source.substr(lineStart, start(ifNode) - lineStart);

    if (!isBlank(indent))
      return false;

    // Between the branch and the else, only a comment on the branch's last
    // line, which stays there.
    TSPoint from = ts_node_end_point(consequence);
    const uint32_t count = childCountOf(ifNode);
    bool comment = false;

    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = nthChild(ifNode, i);

      if (start(child) < end(consequence))
        continue;

      if (same(child, elseClause))
        break;

      if (comment || !isType(child, "comment") ||
          ts_node_start_point(child).row != from.row)
        return false;

      comment = true;
      from = ts_node_end_point(child);
    }

    // A comment in the else clause would be left on the else's line.
    if (namedCount(elseClause) != 1)
      return false;

    // Unindenting would change a literal's text.
    bool multiLineText = false;
    lint::walkTree(body,
                   [&](TSNode node)
                   {
                     if (!isType(node, "string_literal") &&
                         !isType(node, "raw_string_literal"))
                       return Next::Descend;

                     multiLineText |= ts_node_start_point(node).row !=
                                      ts_node_end_point(node).row;
                     return multiLineText ? Next::Stop : Next::Skip;
                   });

    if (multiLineText)
      return false;

    // The else's declarations join the enclosing block: they mustn't clash
    // with its names or change what code after the if means.
    const TSNode enclosing = parent(ifNode);
    const bool compound = isType(body, "compound_statement");
    names.clear();

    if (compound)
      for (uint32_t i = 0; i < namedCount(body); ++i)
      {
        const TSNode statement = namedChild(body, i);

        if (isType(statement, "case_statement"))
          return false;

        addDeclared(statement, names);
      }
    else
    {
      addDeclared(body, names);

      if (!names.empty())
        return false;
    }

    if (!names.empty() && !declarationsMayMove(ifNode, enclosing))
      return false;

    // The if's own declarations stay with it.
    bool usesOwn = false;
    lint::walkTree(lint::LintFile::field(ifNode, "condition"),
                   [&](TSNode node)
                   {
                     if (isType(node, "identifier") && isDeclaredName(node) &&
                         usesWord(text(node), start(body), end(body)))
                       usesOwn = true;

                     return usesOwn ? Next::Stop : Next::Descend;
                   });

    if (usesOwn)
      return false;

    const std::string_view lineBreak = lineBreakAt(source, start(ifNode));
    std::string content;

    if (compound)
    {
      // Nothing may follow the closing brace on its line.
      const std::size_t lineEnd = source.find('\n', end(body));

      if (!isBlank(source.substr(end(body),
                                 lineEnd == std::string_view::npos
                                     ? std::string_view::npos
                                     : lineEnd - end(body))))
        return false;

      const std::string_view inner =
          source.substr(start(body) + 1, end(body) - start(body) - 2);
      const std::size_t firstBreak = inner.find('\n');
      const std::string_view head = inner.substr(0, firstBreak);

      if (!isBlank(head))
      {
        if (firstBreak != std::string_view::npos)
          return false;

        content = std::string(indent) + std::string(lint::trimmed(head));
      }
      else if (firstBreak != std::string_view::npos)
      {
        std::string_view lines = inner.substr(firstBreak + 1);

        while (!lines.empty() && lint::isSpace(lines.back()))
          lines.remove_suffix(1);

        if (!lines.empty() && !reindented(lines, indent, lineBreak, content))
          return false;
      }
    }
    else if (ts_node_start_point(body).row ==
             ts_node_start_point(elseClause).row)
      content = std::string(indent) + std::string(text(body));
    else
    {
      const uint32_t bodyLine = lineStartOf(source, start(body));

      if (!reindented(source.substr(bodyLine, end(body) - bodyLine),
                      indent,
                      lineBreak,
                      content))
        return false;
    }

    const TSPoint to = ts_node_end_point(body);
    std::string replacement;

    if (!content.empty())
    {
      replacement.reserve(2 * lineBreak.size() + content.size());
      replacement += lineBreak;
      replacement += lineBreak;
      replacement += content;
    }

    edits.push_back({SourceRange{from.row, from.column, to.row, to.column},
                     std::move(replacement)});
    return true;
  }

  // Whether `names`, declared in an else body, may move into the block
  // around `ifNode`: none is declared in the block before the if, by the
  // statement that owns the block (a function's parameters, a loop's
  // variables), or written in the block after the if.
  bool declarationsMayMove(TSNode ifNode, TSNode enclosing)
  {
    if (!isType(enclosing, "compound_statement"))
      return false;

    std::vector<std::string_view> before;

    for (uint32_t i = 0; i < namedCount(enclosing); ++i)
    {
      const TSNode statement = namedChild(enclosing, i);

      if (same(statement, ifNode))
        break;

      addDeclared(statement, before);
    }

    if (std::any_of(before.begin(),
                    before.end(),
                    [&](std::string_view name) { return named(name); }))
      return false;

    bool clash = false;
    lint::walkTree(parent(enclosing),
                   [&](TSNode node)
                   {
                     if (isType(node, "compound_statement"))
                       return Next::Skip;

                     clash |= isType(node, "identifier") &&
                              isDeclaredName(node) && named(text(node));
                     return clash ? Next::Stop : Next::Descend;
                   });

    if (clash)
      return false;

    return std::none_of(names.begin(),
                        names.end(),
                        [&](std::string_view name) {
                          return usesWord(name, end(ifNode), end(enclosing));
                        });
  }

  void checkElse(TSNode ifNode)
  {
    const TSNode enclosing = parent(ifNode);

    if (!isType(enclosing, "compound_statement") &&
        !isType(enclosing, "case_statement"))
      return;

    const uint32_t count = childCountOf(ifNode);

    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode token = nthChild(ifNode, i);

      if (isType(token, "constexpr") || isType(token, "consteval"))
        return;
    }

    const TSNode elseClause = lint::LintFile::field(ifNode, "alternative");
    const TSNode consequence = lint::LintFile::field(ifNode, "consequence");

    if (ts_node_is_null(elseClause))
      return;

    const std::string_view jump = endingJump(consequence);

    if (jump.empty())
      return;

    const TSNode body = elseBody(elseClause);

    if (ts_node_is_null(body))
      return;

    std::string message = "The `if` branch ends in `" + std::string(jump) +
                          "`, so this `else` isn't needed. Drop it and write "
                          "its code after the `if`, so the main path isn't "
                          "nested.";
    std::vector<TextEdit> edits;
    const TSNode keyword = nthChild(elseClause, 0);

    if (elseFix(ifNode, consequence, elseClause, body, edits))
      report(RuleId::ElseAfterReturn,
             keyword,
             std::move(message),
             "Remove the else",
             std::move(edits));
    else
      report(RuleId::ElseAfterReturn, keyword, std::move(message));
  }
};

// Every tree-sitter symbol's kind, so aliased symbols match too.
const std::vector<HouseKind>& houseKinds()
{
  static const std::vector<HouseKind> kinds = []
  {
    static const std::pair<std::string_view, HouseKind> names[] = {
        {"conditional_expression", HouseKind::Conditional},
        {"lambda_expression", HouseKind::Lambda},
        {"declaration", HouseKind::Declaration},
        {"field_declaration", HouseKind::Declaration},
        {"if_statement", HouseKind::If},
    };
    const TSLanguage* language = tree_sitter_cpp();
    const uint32_t count = ts_language_symbol_count(language);
    std::vector<HouseKind> made(count, HouseKind::Other);

    for (uint32_t symbol = 0; symbol < count; ++symbol)
    {
      if (ts_language_symbol_type(language, static_cast<TSSymbol>(symbol)) !=
          TSSymbolTypeRegular)
        continue;

      const std::string_view name =
          ts_language_symbol_name(language, static_cast<TSSymbol>(symbol));

      for (const auto& [known, kind] : names)
        if (name == known)
          made[symbol] = kind;
    }

    return made;
  }();
  return kinds;
}

bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

} // namespace

std::string upperSnakeName(std::string_view name)
{
  if (name.size() >= 2 && name[0] == 'k' && isUpper(name[1]))
    name.remove_prefix(1);

  std::string out;
  out.reserve(name.size() + 4);

  for (std::size_t i = 0; i < name.size(); ++i)
  {
    const char c = name[i];

    if (c == '_')
    {
      if (out.empty() || out.back() != '_')
        out += '_';

      continue;
    }

    if (isUpper(c) && !out.empty() && out.back() != '_')
    {
      const char before = name[i - 1];
      const char after = i + 1 < name.size() ? name[i + 1] : '\0';

      // A word starts after a lower-case letter, and at an acronym's or a
      // number's last capital when a lower-case letter follows it.
      if (isLower(before) ||
          ((isUpper(before) || isDigit(before)) && isLower(after)))
        out += '_';
    }

    out += isLower(c) ? static_cast<char>(c - 'a' + 'A') : c;
  }

  return out;
}

void runHouseLints(std::string_view source,
                   const TSTree* tree,
                   const std::filesystem::path& path,
                   std::vector<Diagnostic>& out)
{
  if (!tree)
    return;

  HouseRun run;
  run.source = source;
  run.path = &path;
  run.out = &out;
  run.root = ts_tree_root_node(tree);
  run.kinds = &houseKinds();
  lint::walkTree(run.root,
                 [&](TSNode node)
                 {
                   if (ts_node_is_error(node))
                     return Next::Skip;

                   switch (run.kind(node))
                   {
                   case HouseKind::Conditional:
                     run.checkTernary(node);
                     break;
                   case HouseKind::Declaration:
                     run.collectConstants(node);
                     break;
                   case HouseKind::If:
                     run.checkElse(node);
                     break;
                   default:
                     break;
                   }

                   return Next::Descend;
                 });
  run.checkConstants();
}

} // namespace sfs::lsp
