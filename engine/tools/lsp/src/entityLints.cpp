// The entity API rules: has-then-get, is-alive, row-lookup, filter-in-loop,
// unused-row-entity, row-component-copy, setup-in-constructor and
// component-pointer. Each looks around a site the walk found (a hasComponent,
// isAlive or getComponent call, a range-for loop, a constructor) once the walk
// is done, and fixes what it can rewrite exactly. A fix that can't be made
// safely is left out and the warning stands.

#include "document.h"
#include "lintSupport.h"

#include <sfs/codegen/parser.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <system_error>

namespace sfs::lsp::lint
{

using codegen::RuleId;
using codegen::TextEdit;
using Kind = Linter::Kind;

namespace
{

// C++'s keywords and alternative tokens: a binding name made from a type
// can't be one.
constexpr std::array<std::string_view, 92> kKeywords = {
    "alignas",       "alignof",     "and",
    "and_eq",        "asm",         "auto",
    "bitand",        "bitor",       "bool",
    "break",         "case",        "catch",
    "char",          "char8_t",     "char16_t",
    "char32_t",      "class",       "compl",
    "concept",       "const",       "consteval",
    "constexpr",     "constinit",   "const_cast",
    "continue",      "co_await",    "co_return",
    "co_yield",      "decltype",    "default",
    "delete",        "do",          "double",
    "dynamic_cast",  "else",        "enum",
    "explicit",      "export",      "extern",
    "false",         "float",       "for",
    "friend",        "goto",        "if",
    "inline",        "int",         "long",
    "mutable",       "namespace",   "new",
    "noexcept",      "not",         "not_eq",
    "nullptr",       "operator",    "or",
    "or_eq",         "private",     "protected",
    "public",        "register",    "reinterpret_cast",
    "requires",      "return",      "short",
    "signed",        "sizeof",      "static",
    "static_assert", "static_cast", "struct",
    "switch",        "template",    "this",
    "thread_local",  "throw",       "true",
    "try",           "typedef",     "typeid",
    "typename",      "union",       "unsigned",
    "using",         "virtual",     "void",
    "volatile",      "wchar_t",     "while",
    "xor",           "xor_eq"};

template <std::size_t N>
bool listedIn(const std::array<std::string_view, N>& list,
              std::string_view name)
{
  return std::find(list.begin(), list.end(), name) != list.end();
}

// `name` in lower camel case: "CFVehicle" gives "cfVehicle", "Hitch" "hitch".
std::string lowerCamel(std::string_view name)
{
  std::string out(name);
  std::size_t upper = 0;
  while (upper < out.size() && out[upper] >= 'A' && out[upper] <= 'Z')
    ++upper;
  // Keep the last capital of a run that starts the next word: CFVehicle.
  const bool word =
      upper > 1 && upper < out.size() && out[upper] >= 'a' && out[upper] <= 'z';
  for (std::size_t i = 0; i < (word ? upper - 1 : upper); ++i)
    out[i] = static_cast<char>(out[i] - 'A' + 'a');
  return out;
}

// A spelling without a leading `const` (a query term's `const T` is T).
std::string_view unconst(std::string_view spelled)
{
  spelled = trimmed(spelled);
  return spelled.starts_with("const ") ? trimmed(spelled.substr(6)) : spelled;
}

// The name a component's binding gets from its type: its last identifier, in
// lower camel case, without "Component" (sfs::TransformComponent and
// `const sfs::TransformComponent` give "transform"). Empty when that isn't a
// usable name.
std::string nameFor(std::string_view type)
{
  std::string_view plain = unconst(type);
  if (const std::size_t open = plain.find('<'); open != std::string_view::npos)
    plain = plain.substr(0, open);
  plain = lastName(plain);
  if (plain.size() > 9 && plain.ends_with("Component"))
    plain.remove_suffix(9);
  if (plain.empty())
    return {};
  for (const char c : plain)
    if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9')))
      return {};
  std::string name = lowerCamel(plain);
  return listedIn(kKeywords, name) ? std::string() : name;
}

// A file's text with its line starts, to turn byte offsets into the points
// edits are written in.
struct Lines
{
  std::string_view text;
  std::vector<uint32_t> starts;

  void set(std::string_view source)
  {
    text = source;
    starts = countLines(source);
  }

  TSPoint point(uint32_t byte) const
  {
    const auto after = std::upper_bound(starts.begin(), starts.end(), byte);
    const auto row = static_cast<uint32_t>(after - starts.begin() - 1);
    return {row, byte - starts[row]};
  }

  uint32_t lineStart(uint32_t byte) const { return starts[point(byte).row]; }

  // The start of the line after `byte`'s, or the end of the text.
  uint32_t nextLine(uint32_t byte) const
  {
    const uint32_t row = point(byte).row;
    return row + 1 < starts.size() ? starts[row + 1]
                                   : static_cast<uint32_t>(text.size());
  }

  // The whitespace that starts `byte`'s line.
  std::string_view indentOf(uint32_t byte) const
  {
    const uint32_t start = lineStart(byte);
    uint32_t end = start;
    while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
      ++end;
    return text.substr(start, end - start);
  }

  // Whether only whitespace is between the line's start and `byte`.
  bool startsLine(uint32_t byte) const
  {
    for (uint32_t i = lineStart(byte); i < byte; ++i)
      if (text[i] != ' ' && text[i] != '\t')
        return false;
    return true;
  }

  // Whether only whitespace is between `byte` and the line's end.
  bool endsLine(uint32_t byte) const
  {
    for (uint32_t i = byte; i < text.size() && text[i] != '\n'; ++i)
      if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r')
        return false;
    return true;
  }

  TextEdit edit(uint32_t from,
                uint32_t to,
                std::string replacement,
                const std::filesystem::path& file = {}) const
  {
    const TSPoint a = point(from);
    const TSPoint b = point(to);
    return {{a.row, a.column, b.row, b.column}, std::move(replacement), file};
  }
};

// `text` re-indented: the common indentation of its lines replaced by
// `indent`, each line ending in a line break.
std::string reindent(std::string_view text, std::string_view indent)
{
  std::vector<std::string_view> lines;
  for (std::size_t start = 0; start <= text.size();)
  {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos)
      end = text.size();
    std::string_view line = text.substr(start, end - start);
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    lines.push_back(line);
    start = end + 1;
  }
  while (!lines.empty() && trimmed(lines.back()).empty())
    lines.pop_back();
  std::size_t common = std::string_view::npos;
  for (const std::string_view line : lines)
  {
    if (trimmed(line).empty())
      continue;
    const std::size_t lead = line.find_first_not_of(" \t");
    common = std::min(common, lead);
  }
  if (common == std::string_view::npos)
    common = 0;
  std::string out;
  for (const std::string_view line : lines)
  {
    if (!trimmed(line).empty())
    {
      out += indent;
      out += line.substr(std::min(common, line.size()));
    }
    out += '\n';
  }
  return out;
}

// A getComponent<T>() or hasComponent<T>() call: `E.f<T>()`, or the
// registry's `R->f<T>(E)`.
struct Lookup
{
  TSNode call{};
  TSNode name{};     // the called name's node
  TSNode type{};     // T
  TSNode entity{};   // E
  TSNode registry{}; // R in the registry's form, else null
};

struct Rules
{
  LintFile& f;
  Lines lines; // the file's, made when a fix needs them
  bool linesReady = false;

  explicit Rules(LintFile& file) : f(file) {}

  // ------------------------------------------------------------- helpers

  Kind kind(TSNode node) const { return f.kind(node); }
  std::string_view text(TSNode node) const { return f.text(node); }
  static TSNode field(TSNode node, std::string_view name)
  {
    return LintFile::field(node, name);
  }
  static TSNode parent(TSNode node)
  {
    return ts_node_is_null(node) ? TSNode{} : ts_node_parent(node);
  }
  static uint32_t start(TSNode node)
  {
    return ts_node_is_null(node) ? 0 : ts_node_start_byte(node);
  }
  static uint32_t end(TSNode node)
  {
    return ts_node_is_null(node) ? 0 : ts_node_end_byte(node);
  }
  // Tree-sitter's node accessors, made safe on a null node (which they would
  // dereference): no children, no siblings, an empty type.
  static uint32_t namedCount(TSNode node)
  {
    return ts_node_is_null(node) ? 0 : ts_node_named_child_count(node);
  }
  static TSNode namedChild(TSNode node, uint32_t index)
  {
    return index < namedCount(node) ? ts_node_named_child(node, index)
                                    : TSNode{};
  }
  static uint32_t childCountOf(TSNode node)
  {
    return ts_node_is_null(node) ? 0 : ts_node_child_count(node);
  }
  static TSNode nthChild(TSNode node, uint32_t index)
  {
    return index < childCountOf(node) ? ts_node_child(node, index) : TSNode{};
  }
  static TSNode nextNamed(TSNode node)
  {
    return ts_node_is_null(node) ? TSNode{} : ts_node_next_named_sibling(node);
  }
  static TSNode prevNamed(TSNode node)
  {
    return ts_node_is_null(node) ? TSNode{} : ts_node_prev_named_sibling(node);
  }
  static TSNode prevToken(TSNode node)
  {
    return ts_node_is_null(node) ? TSNode{} : ts_node_prev_sibling(node);
  }
  static const char* typeName(TSNode node)
  {
    return ts_node_is_null(node) ? "" : ts_node_type(node);
  }
  static bool same(TSNode a, TSNode b)
  {
    return !ts_node_is_null(a) && !ts_node_is_null(b) && ts_node_eq(a, b);
  }
  static bool within(TSNode inner, TSNode outer)
  {
    return !ts_node_is_null(outer) && start(outer) <= start(inner) &&
           end(inner) <= end(outer);
  }

  const Lines& fileLines()
  {
    if (!linesReady)
    {
      lines.set(f.source);
      linesReady = true;
    }
    return lines;
  }

  TextEdit replace(TSNode node, std::string replacement)
  {
    return fileLines().edit(start(node), end(node), std::move(replacement));
  }

  // The unnamed operator token of a field or pointer expression ("." / "->").
  TSNode operatorOf(TSNode node) const
  {
    const TSNode op = field(node, "operator");
    if (!ts_node_is_null(op))
      return op;
    const uint32_t count = childCountOf(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = nthChild(node, i);
      if (!ts_node_is_named(child))
        return child;
    }
    return {};
  }

  // `node` without the parentheses around it.
  TSNode unparenthesised(TSNode node) const
  {
    while (kind(node) == Kind::ParenthesizedExpression && namedCount(node) == 1)
      node = namedChild(node, 0);
    return node;
  }

  // `node`'s parent, past the parentheses around `node`.
  TSNode outerParent(TSNode& node) const
  {
    TSNode up = parent(node);
    while (kind(up) == Kind::ParenthesizedExpression)
    {
      node = up;
      up = parent(up);
    }
    return up;
  }

  bool isBinary(TSNode node, std::string_view op) const
  {
    return kind(node) == Kind::BinaryExpression &&
           text(field(node, "operator")) == op;
  }

  bool isNot(TSNode node) const
  {
    return kind(node) == Kind::UnaryExpression &&
           text(field(node, "operator")) == "!";
  }

  // The operands of a chain of `op` (a && b && c), left to right, through
  // parentheses.
  void operands(TSNode node, std::string_view op, std::vector<TSNode>& out)
  {
    const TSNode inner = unparenthesised(node);
    if (isBinary(inner, op))
    {
      operands(field(inner, "left"), op, out);
      operands(field(inner, "right"), op, out);
    }
    else
      out.push_back(node);
  }

  // An identifier, `this`, or a chain of fields of one (`hitch.implement`,
  // `this->m_x`, `wheel->dual`): an entity expression that names the same
  // handle each time it's read.
  bool plainChain(TSNode node) const
  {
    switch (kind(node))
    {
    case Kind::Identifier:
    case Kind::This:
      return true;
    case Kind::FieldExpression:
      return kind(field(node, "field")) == Kind::FieldIdentifier &&
             plainChain(field(node, "argument"));
    default:
      return false;
    }
  }

  // The identifier a plain chain starts with ("hitch" of hitch.implement), or
  // empty for `this`.
  std::string_view rootOf(TSNode node) const
  {
    while (kind(node) == Kind::FieldExpression)
      node = field(node, "argument");
    return kind(node) == Kind::Identifier ? text(node) : std::string_view{};
  }

  bool lookupOf(TSNode call, Lookup& out) const
  {
    TSNode function = field(call, "function");
    if (kind(function) != Kind::FieldExpression)
      return false;
    const TSNode method = field(function, "field");
    if (kind(method) != Kind::TemplateMethod)
      return false;
    const TSNode templateArguments = field(method, "arguments");
    if (namedCount(templateArguments) != 1)
      return false;
    out.call = call;
    out.name = field(method, "name");
    out.type = namedChild(templateArguments, 0);
    const TSNode arguments = field(call, "arguments");
    const uint32_t count = namedCount(arguments);
    const TSNode receiver = field(function, "argument");
    if (count == 0)
    {
      out.entity = receiver;
      out.registry = TSNode{};
    }
    else if (count == 1)
    {
      out.entity = namedChild(arguments, 0);
      out.registry = receiver;
    }
    else
      return false;
    return plainChain(out.entity);
  }

  // Whether two lookups are of one component on one handle, written alike.
  bool sameLookup(const Lookup& a, const Lookup& b) const
  {
    return ts_node_is_null(a.registry) == ts_node_is_null(b.registry) &&
           sameSpelling(text(a.entity), text(b.entity)) &&
           sameSpelling(text(a.type), text(b.type)) &&
           (ts_node_is_null(a.registry) ||
            sameSpelling(text(a.registry), text(b.registry)));
  }

  // `has`'s call with getComponent in place of hasComponent.
  std::string getText(const Lookup& has) const
  {
    std::string out(
        f.source.substr(start(has.call), start(has.name) - start(has.call)));
    out += "getComponent";
    out += f.source.substr(end(has.name), end(has.call) - end(has.name));
    return out;
  }

  // ---------------------------------------------------- names and scopes

  // Whether `node` is the name a declarator declares.
  bool declaresHere(TSNode node) const
  {
    const TSNode up = parent(node);
    switch (kind(up))
    {
    case Kind::ReferenceDeclarator:
    case Kind::PointerDeclarator:
    case Kind::StructuredBindingDeclarator:
      return true;
    case Kind::InitDeclarator:
    case Kind::Declaration:
    case Kind::ParameterDeclaration:
    case Kind::OptionalParameterDeclaration:
    case Kind::ForRangeLoop:
      return same(field(up, "declarator"), node);
    case Kind::LambdaCaptureInitializer:
      return same(field(up, "left"), node);
    default:
      return false;
    }
  }

  // The scope a declared name lives in: its block, loop, condition, lambda or
  // function.
  TSNode scopeOfDeclaration(TSNode name) const
  {
    for (TSNode up = parent(name); !ts_node_is_null(up); up = parent(up))
      switch (kind(up))
      {
      case Kind::CompoundStatement:
      case Kind::ForRangeLoop:
      case Kind::ForStatement:
      case Kind::LambdaExpression:
      case Kind::IfStatement:
      case Kind::WhileStatement:
      case Kind::SwitchStatement:
      case Kind::CatchClause:
      case Kind::FunctionDefinition:
      case Kind::FieldDeclarationList:
      case Kind::TranslationUnit:
        return up;
      default:
        break;
      }
    return {};
  }

  // Whether a declaration-like node declares `name`.
  bool declares(TSNode node, std::string_view name) const
  {
    switch (kind(node))
    {
    case Kind::Declaration:
    case Kind::FieldDeclaration:
    case Kind::ParameterDeclaration:
    case Kind::OptionalParameterDeclaration:
    {
      const uint32_t count = namedCount(node);
      for (uint32_t i = 0; i < count; ++i)
      {
        const char* fieldName = ts_node_field_name_for_named_child(node, i);
        if (!fieldName || std::strcmp(fieldName, "declarator") != 0)
          continue;
        const TSNode declarator = namedChild(node, i);
        if (kind(declarator) == Kind::StructuredBindingDeclarator
                ? unpacks(declarator, name)
                : f.declaredName(declarator) == name)
          return true;
      }
      return false;
    }
    case Kind::InitStatement:
    case Kind::ConditionClause:
    {
      const uint32_t count = namedCount(node);
      for (uint32_t i = 0; i < count; ++i)
        if (declares(namedChild(node, i), name))
          return true;
      return false;
    }
    default:
      return false;
    }
  }

  // Whether a structured binding declarator binds `name`.
  bool unpacks(TSNode declarator, std::string_view name) const
  {
    if (kind(declarator) != Kind::StructuredBindingDeclarator)
      return false;
    const uint32_t names = namedCount(declarator);
    for (uint32_t n = 0; n < names; ++n)
      if (text(namedChild(declarator, n)) == name)
        return true;
    return false;
  }

  // The declaration in a condition or init-statement that declares `name`.
  TSNode declaringIn(TSNode clause, std::string_view name) const
  {
    if (kind(clause) != Kind::InitStatement &&
        kind(clause) != Kind::ConditionClause)
      return clause;
    const uint32_t count = namedCount(clause);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = namedChild(clause, i);
      if (declares(child, name))
        return declaringIn(child, name);
    }
    return clause;
  }

  // The parameter of a list that declares `name`, or null.
  TSNode parameterNamed(TSNode parameters, std::string_view name) const
  {
    const uint32_t count =
        ts_node_is_null(parameters) ? 0 : namedCount(parameters);
    for (uint32_t i = 0; i < count; ++i)
      if (declares(namedChild(parameters, i), name))
        return namedChild(parameters, i);
    return {};
  }

  // The parameter list of a function definition, or null.
  TSNode parametersOf(TSNode function) const
  {
    return field(functionDeclarator(function), "parameters");
  }

  // The field declaration of `name` in a class body, or null.
  TSNode fieldOf(TSNode classNode, std::string_view name) const
  {
    const TSNode body = field(classNode, "body");
    const uint32_t count = ts_node_is_null(body) ? 0 : namedCount(body);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode member = namedChild(body, i);
      if (kind(member) == Kind::FieldDeclaration && declares(member, name))
        return member;
    }
    return {};
  }

  /*
   * The node declaring `name` that's visible at `at`: a declaration earlier
   * in an enclosing block, a loop's or condition's variable, a lambda's or
   * function's parameter, or a field of the enclosing class written in this
   * file. Null when there's none.
   */
  TSNode declarationOf(TSNode at, std::string_view name) const
  {
    TSNode child = at;
    for (TSNode up = parent(at); !ts_node_is_null(up);
         child = up, up = parent(up))
    {
      switch (kind(up))
      {
      case Kind::CompoundStatement:
      {
        const uint32_t count = namedCount(up);
        for (uint32_t i = 0; i < count; ++i)
        {
          const TSNode statement = namedChild(up, i);
          if (start(statement) >= start(child))
            break;
          if (declares(statement, name))
            return statement;
        }
        break;
      }
      case Kind::ForRangeLoop:
        if (same(child, field(up, "body")) &&
            (f.declaredName(field(up, "declarator")) == name ||
             unpacks(field(up, "declarator"), name)))
          return up;
        break;
      case Kind::ForStatement:
      {
        const TSNode initializer = field(up, "initializer");
        if (!same(child, initializer) && declares(initializer, name))
          return declaringIn(initializer, name);
        break;
      }
      case Kind::IfStatement:
      case Kind::WhileStatement:
      case Kind::SwitchStatement:
      {
        const TSNode condition = field(up, "condition");
        if (!same(child, condition) && declares(condition, name))
          return declaringIn(condition, name);
        break;
      }
      case Kind::ConditionClause:
      {
        const TSNode initializer = field(up, "initializer");
        if (!same(child, initializer) && declares(initializer, name))
          return declaringIn(initializer, name);
        break;
      }
      case Kind::LambdaExpression:
        if (const TSNode parameter = parameterNamed(
                field(field(up, "declarator"), "parameters"), name);
            !ts_node_is_null(parameter))
          return parameter;
        break;
      case Kind::CatchClause:
        if (const TSNode parameter =
                parameterNamed(field(up, "parameters"), name);
            !ts_node_is_null(parameter))
          return parameter;
        break;
      case Kind::FunctionDefinition:
      {
        if (const TSNode parameter = parameterNamed(parametersOf(up), name);
            !ts_node_is_null(parameter))
          return parameter;
        // An out-of-line method sees its class's fields.
        if (const FileClass* owner = f.fileClass(f.ownerAt(child)))
          if (const TSNode member = fieldOf(owner->node, name);
              !ts_node_is_null(member))
            return member;
        break;
      }
      case Kind::FieldDeclarationList:
      {
        const TSNode member = fieldOf(parent(up), name);
        if (!ts_node_is_null(member))
          return member;
        break;
      }
      default:
        break;
      }
    }
    return {};
  }

  // The type node of a declaration-like node (its `type` field), or null.
  TSNode typeOf(TSNode declaration) const { return field(declaration, "type"); }

  // Whether `scope` mentions the identifier `name` from the byte `from` on,
  // outside `skip` (which may be null).
  bool mentions(TSNode scope,
                std::string_view name,
                TSNode skip = TSNode{},
                uint32_t from = 0) const
  {
    bool found = false;
    walkTree(scope,
             [&](TSNode node)
             {
               if (same(node, skip) || end(node) <= from)
                 return Next::Skip;
               found = kind(node) == Kind::Identifier && start(node) >= from &&
                       text(node) == name;
               return found ? Next::Stop : Next::Descend;
             });
    return found;
  }

  // Whether `node` is inside a lambda that's inside `outer`.
  bool inLambdaWithin(TSNode node, TSNode outer) const
  {
    for (TSNode up = parent(node); !ts_node_is_null(up) && !same(up, outer);
         up = parent(up))
      if (kind(up) == Kind::LambdaExpression)
        return true;
    return false;
  }

  // Whether `node` is where a component being removed can still be looked
  // up but hasComponent() already says no: onEntityRemoved() of a system, an
  // observe() removed callback, or what runs as an excluded component goes
  // (an observe() added callback of terms with an Excluding<...>, or
  // onEntityAdded() of a system registering one).
  bool inRemovalHook(TSNode node)
  {
    for (TSNode up = parent(node); !ts_node_is_null(up); up = parent(up))
    {
      if (kind(up) == Kind::LambdaExpression)
      {
        const TSNode arguments = parent(up);
        TSNode templateArguments;
        if (kind(arguments) == Kind::ArgumentList &&
            f.calleeName(parent(arguments), templateArguments) == "observe" &&
            (same(namedChild(arguments, 1), up) ||
             (same(namedChild(arguments, 0), up) &&
              excludes(templateArguments, up, f.ownerAt(up)))))
          return true;
      }
      if (kind(up) != Kind::FunctionDefinition)
        continue;
      TSNode name = memberName(up);
      while (kind(name) == Kind::QualifiedIdentifier)
        name = field(name, "name");
      if (text(name) != "onEntityAdded")
        return text(name) == "onEntityRemoved";
      const std::string_view owner = f.ownerAt(up);
      m_registered.clear();
      m_registeredBound.clear();
      m_excludedScratch.clear();
      if (!registeredOf(owner, up, m_registered))
        return false;
      for (const std::string_view term : m_registered)
        boundTerms(up, owner, term, m_registeredBound, m_excludedScratch);
      return !m_excludedScratch.empty();
    }
    return false;
  }

  // Whether a template argument list excludes a component (an
  // Excluding<...>, aliases expanded).
  bool excludes(TSNode templateArguments, TSNode at, std::string_view owner)
  {
    m_registeredBound.clear();
    m_excludedScratch.clear();
    for (uint32_t i = 0; i < namedCount(templateArguments); ++i)
      boundTerms(at,
                 owner,
                 text(namedChild(templateArguments, i)),
                 m_registeredBound,
                 m_excludedScratch);
    return !m_excludedScratch.empty();
  }

  // Whether `entity` (a plain chain) may name another handle somewhere in
  // `scope` between the bytes `from` and `to`: assigned, or its first name
  // declared again.
  bool changedBetween(TSNode entity, TSNode scope, uint32_t from, uint32_t to)
  {
    const std::string_view root = rootOf(entity);
    bool changed = false;
    walkTree(scope,
             [&](TSNode node)
             {
               if (end(node) <= from || start(node) >= to)
                 return Next::Skip;
               const Kind k = kind(node);
               const TSNode changes =
                   k == Kind::AssignmentExpression ? field(node, "left")
                   : k == Kind::UpdateExpression   ? field(node, "argument")
                                                   : TSNode{};
               changed = (!ts_node_is_null(changes) &&
                          (sameSpelling(text(changes), text(entity)) ||
                           (!root.empty() && text(changes) == root))) ||
                         (!root.empty() && k == Kind::Identifier &&
                          start(node) >= from && text(node) == root &&
                          declaresHere(node));
               return changed ? Next::Stop : Next::Descend;
             });
    return changed;
  }

  // The function, lambda or file a node is in.
  TSNode functionOf(TSNode node) const
  {
    for (TSNode up = parent(node); !ts_node_is_null(up); up = parent(up))
      if (kind(up) == Kind::FunctionDefinition ||
          kind(up) == Kind::LambdaExpression)
        return up;
    return {};
  }

  // ------------------------------------------------------ rewriting uses

  // Whether `call` initialises a pointer: `T* x = call`, or a condition's
  // `if (T* x = call)`.
  bool initialisesPointer(TSNode call) const
  {
    const TSNode up = parent(call);
    if ((kind(up) != Kind::InitDeclarator && kind(up) != Kind::Declaration) ||
        !same(field(up, "value"), call))
      return false;
    return kind(field(up, "declarator")) == Kind::PointerDeclarator;
  }

  // Whether a replacement of `node` must be parenthesised to keep binding
  // first: it's the object of a postfix expression.
  bool postfixObject(TSNode node) const
  {
    const TSNode up = parent(node);
    switch (kind(up))
    {
    case Kind::SubscriptExpression:
      return same(field(up, "argument"), node);
    case Kind::CallExpression:
      return same(field(up, "function"), node);
    case Kind::FieldExpression:
      return same(field(up, "argument"), node);
    case Kind::UpdateExpression:
      return true;
    default:
      return false;
    }
  }

  /*
   * The edits that make a lookup read through `name` instead: a pointer to
   * its component (`pointer`), or a reference to it. The lookup is read as
   * the old reference (`.f`, `&`, a value) or as a pointer already (`->f`,
   * `*`, a pointer's initialiser).
   */
  void readThrough(TSNode call,
                   std::string_view name,
                   bool pointer,
                   std::vector<TextEdit>& edits)
  {
    const std::string n(name);
    const TSNode up = parent(call);
    if (kind(up) == Kind::FieldExpression && same(field(up, "argument"), call))
    {
      const TSNode op = operatorOf(up);
      const bool arrow = text(op) == "->";
      if (arrow == pointer)
        edits.push_back(replace(call, n));
      else
        edits.push_back(
            fileLines().edit(start(call), end(op), n + (pointer ? "->" : ".")));
      return;
    }
    if (kind(up) == Kind::PointerExpression &&
        same(field(up, "argument"), call))
    {
      const bool address = text(field(up, "operator")) == "&";
      if (address)
        edits.push_back(replace(up, pointer ? n : "&" + n));
      else
        edits.push_back(replace(up, pointer ? "*" + n : n));
      return;
    }
    if (initialisesPointer(call))
    {
      edits.push_back(replace(call, pointer ? n : "&" + n));
      return;
    }
    if (!pointer)
      edits.push_back(replace(call, n));
    else if (postfixObject(call))
      edits.push_back(replace(call, "(*" + n + ")"));
    else
      edits.push_back(replace(call, "*" + n));
  }

  // The edit that makes one use of a reference variable read through it as a
  // pointer.
  void pointerUse(TSNode use, std::vector<TextEdit>& edits)
  {
    const std::string name(text(use));
    const TSNode up = parent(use);
    if (kind(up) == Kind::FieldExpression && same(field(up, "argument"), use))
    {
      const TSNode op = operatorOf(up);
      if (text(op) == ".")
        edits.push_back(replace(op, "->"));
      else
        edits.push_back(replace(use, "(*" + name + ")"));
      return;
    }
    if (kind(up) == Kind::PointerExpression &&
        text(field(up, "operator")) == "&")
    {
      edits.push_back(replace(up, name));
      return;
    }
    edits.push_back(
        replace(use, postfixObject(use) ? "(*" + name + ")" : "*" + name));
  }

  // Whether a lambda between `node` and `scope` captures by copy everything
  // it uses (`[=]`).
  bool copiesByDefault(TSNode node, TSNode scope) const
  {
    for (TSNode up = parent(node); !ts_node_is_null(up) && !same(up, scope);
         up = parent(up))
    {
      if (kind(up) != Kind::LambdaExpression)
        continue;
      const TSNode captures = field(up, "captures");
      const uint32_t count =
          ts_node_is_null(captures) ? 0 : namedCount(captures);
      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode capture = namedChild(captures, i);
        if (kind(capture) == Kind::LambdaDefaultCapture &&
            trimmed(text(capture)) == "=")
          return true;
      }
    }
    return false;
  }

  /*
   * The edits that turn a reference variable `name` into a pointer, at each
   * of its uses in `scope` after the byte `from` that its declaration still
   * reaches. False when a lambda captures it by copy: a pointer copied there
   * wouldn't copy the component.
   */
  bool pointerUses(TSNode scope,
                   uint32_t from,
                   std::string_view name,
                   std::vector<TextEdit>& edits)
  {
    std::vector<uint32_t> shadows; // where a redeclaration's scope ends
    bool ok = true;
    walkTree(
        scope,
        [&](TSNode node)
        {
          while (!shadows.empty() && start(node) >= shadows.back())
            shadows.pop_back();
          if (end(node) <= from)
            return Next::Skip;
          if (start(node) < from || kind(node) != Kind::Identifier ||
              text(node) != name)
            return Next::Descend;
          const TSNode up = parent(node);
          if (declaresHere(node))
          {
            const TSNode inner = scopeOfDeclaration(node);
            if (!ts_node_is_null(inner))
              shadows.push_back(end(inner));
          }
          else if (!shadows.empty() || kind(up) == Kind::QualifiedIdentifier)
          {
          }
          else if (kind(up) == Kind::LambdaCaptureSpecifier)
          {
            const TSNode before = prevToken(node);
            ok = !ts_node_is_null(before) && text(before) == "&";
          }
          else if (copiesByDefault(node, scope))
            ok = false;
          else
            pointerUse(node, edits);
          return ok ? Next::Descend : Next::Stop;
        });
    return ok;
  }

  // The scope a block-level declaration's name reaches to the end of: its
  // block, or the statement whose init-statement or condition it's in.
  TSNode scopeOfStatement(TSNode declaration) const
  {
    TSNode up = parent(declaration);
    while (kind(up) == Kind::InitStatement || kind(up) == Kind::ConditionClause)
      up = parent(up);
    return up;
  }

  // The edit removing a statement: its whole lines when nothing else is on
  // them, else just its text.
  TextEdit removeStatement(TSNode statement)
  {
    const Lines& text = fileLines();
    const uint32_t from = start(statement);
    const uint32_t to = end(statement);
    if (text.startsLine(from) && text.endsLine(to))
      return text.edit(text.lineStart(from), text.nextLine(to), {});
    uint32_t after = to;
    while (after < f.source.size() && f.source[after] == ' ')
      ++after;
    return text.edit(from, after, {});
  }

  // For `T& x = <call>;`, a declaration of one reference bound to the call:
  // the declaration, else null.
  TSNode boundDeclaration(TSNode call) const
  {
    const TSNode init = parent(call);
    if (kind(init) != Kind::InitDeclarator || !same(field(init, "value"), call))
      return {};
    const TSNode declarator = field(init, "declarator");
    if (kind(declarator) != Kind::ReferenceDeclarator ||
        text(declarator).starts_with("&&"))
      return {};
    const TSNode declaration = parent(init);
    if (kind(declaration) != Kind::Declaration)
      return {};
    return declaratorCount(declaration) == 1 ? declaration : TSNode{};
  }

  // Whether a declaration's type is const (`const T&`, `const auto&`).
  bool constType(TSNode declaration) const
  {
    const uint32_t count = namedCount(declaration);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = namedChild(declaration, i);
      if (std::strcmp(typeName(child), "type_qualifier") == 0 &&
          text(child) == "const")
        return true;
    }
    return false;
  }

  // -------------------------------------------------------------- is-alive

  // Whether a declaration-like node declares an SFS Entity.
  bool entityTyped(TSNode declaration, TSNode at) const
  {
    const TSNode type = typeOf(declaration);
    return !ts_node_is_null(type) && f.isSfsNameAt(at, text(type), "Entity");
  }

  // Whether `receiver` is known to be an sfs::Entity: a variable declared so
  // in scope, or a field declared so in one of the file's classes.
  bool knownEntity(TSNode receiver) const
  {
    if (kind(receiver) == Kind::Identifier)
    {
      const TSNode declaration = declarationOf(receiver, text(receiver));
      if (!ts_node_is_null(declaration))
        return entityTyped(declaration, receiver);
    }
    const std::string_view name =
        kind(receiver) == Kind::FieldExpression
            ? text(field(receiver, "field"))
            : (kind(receiver) == Kind::Identifier ? text(receiver)
                                                  : std::string_view{});
    if (name.empty())
      return false;
    for (const FileClass& owner : f.fileClasses)
      if (const TSNode member = fieldOf(owner.node, name);
          !ts_node_is_null(member))
        return entityTyped(member, receiver);
    return false;
  }

  void isAlive(TSNode call)
  {
    const TSNode function = field(call, "function");
    if (kind(function) != Kind::FieldExpression)
      return;
    const TSNode name = field(function, "field");
    const TSNode receiver = field(function, "argument");
    const TSNode arguments = field(call, "arguments");
    const uint32_t count = namedCount(arguments);
    const std::string message =
        "Entity::isAlive() is gone, because a handle is now valid only while "
        "its entity lives. Use isValid(), or test the handle itself with "
        "if (entity).";
    if (count == 1)
    {
      // The registry's isAlive(entity).
      std::string_view spelled = trimmed(text(receiver));
      if (!spelled.ends_with("registry") && !spelled.ends_with("Registry") &&
          !spelled.ends_with("scene()"))
        return;
      f.report(RuleId::IsAlive,
               call,
               message,
               "Change to isValid()",
               {replace(name, "isValid")});
      return;
    }
    if (count != 0)
      return;

    // Paired with isValid() on the same handle: the pair goes to one test.
    TSNode test = call;
    const bool negated = isNot(parent(call));
    if (negated)
      test = parent(call);
    TSNode outer = test;
    const TSNode up = outerParent(outer);
    if (isBinary(up, negated ? "||" : "&&") && same(field(up, "right"), outer))
    {
      std::vector<TSNode> before;
      operands(field(up, "left"), negated ? "||" : "&&", before);
      TSNode valid = unparenthesised(before.back());
      if (negated && isNot(valid))
        valid = field(valid, "argument");
      else if (negated)
        valid = TSNode{};
      TSNode validArguments;
      if (!ts_node_is_null(valid) && kind(valid) == Kind::CallExpression &&
          f.calleeName(valid, validArguments) == "isValid" &&
          namedCount(field(valid, "arguments")) == 0 &&
          sameSpelling(text(f.receiverOf(valid)), text(receiver)))
      {
        f.report(RuleId::IsAlive,
                 call,
                 message,
                 negated ? "Drop the || !isAlive()" : "Drop the && isAlive()",
                 {fileLines().edit(end(before.back()), end(outer), {})});
        return;
      }
    }
    // `X.isValid() && !X.isAlive()` asks whether a handle that was an entity
    // is dead; renamed alone it would never hold. The validity test becomes
    // `X != sfs::Entity{}`. (And `!X.isValid() || X.isAlive()`, its negation.)
    const std::string spelled = tidy(text(receiver));
    if (const TSNode validity = validityBefore(outer, receiver, negated);
        !ts_node_is_null(validity))
    {
      f.report(
          RuleId::IsAlive,
          call,
          message,
          "Test against sfs::Entity{} and isValid()",
          {replace(validity,
                   spelled + (negated ? " != " : " == ") + "sfs::Entity{}"),
           replace(name, "isValid")});
      return;
    }
    if (negated && validityGuards(call, receiver))
    {
      // Under an earlier validity test the renamed test would never hold, and
      // that test may need to become `X != sfs::Entity{}`: a hand fix.
      f.report(RuleId::IsAlive,
               call,
               "Entity::isAlive() is gone, and here it follows " + spelled +
                   ".isValid(), so it asks whether a handle that was an "
                   "entity is dead. Write " +
                   spelled + " != sfs::Entity{} && !" + spelled +
                   ".isValid(), and check what the earlier test means now.");
      return;
    }
    if (!knownEntity(receiver))
      return;
    f.report(RuleId::IsAlive,
             call,
             message,
             "Change to isValid()",
             {replace(name, "isValid")});
  }

  // Whether `node` is `X.isValid()` (or with `negated`, `!X.isValid()`) for
  // the handle `receiver`.
  bool isValidOf(TSNode node, TSNode receiver, bool negated) const
  {
    node = unparenthesised(node);
    if (negated)
    {
      if (!isNot(node))
        return false;
      node = unparenthesised(field(node, "argument"));
    }
    TSNode templateArguments;
    return kind(node) == Kind::CallExpression &&
           f.calleeName(node, templateArguments) == "isValid" &&
           namedCount(field(node, "arguments")) == 0 &&
           sameSpelling(text(f.receiverOf(node)), text(receiver));
  }

  // For `!X.isAlive()` (`negated`) in an && chain, an earlier `X.isValid()`
  // operand; for `X.isAlive()` in an || chain, an earlier `!X.isValid()`.
  // Null when there's none.
  TSNode validityBefore(TSNode test, TSNode receiver, bool negated)
  {
    const std::string_view op = negated ? "&&" : "||";
    TSNode top = test;
    for (TSNode up = outerParent(top); isBinary(up, op); up = outerParent(top))
      top = up;
    if (same(top, test))
      return {};
    std::vector<TSNode> chain;
    operands(top, op, chain);
    for (const TSNode operand : chain)
    {
      if (within(test, operand))
        break;
      if (isValidOf(operand, receiver, !negated))
        return unparenthesised(operand);
    }
    return {};
  }

  // Whether `X.isValid()` holds where `call` is: it's in the code an if or a
  // ternary with an `X.isValid()` && operand guards, or after an early-out
  // on `!X.isValid()` in an enclosing block.
  bool validityGuards(TSNode call, TSNode receiver)
  {
    const auto tests = [&](TSNode condition, bool negated, std::string_view op)
    {
      std::vector<TSNode> chain;
      operands(condition, op, chain);
      for (const TSNode operand : chain)
        if (isValidOf(operand, receiver, negated))
          return true;
      return false;
    };
    TSNode child = call;
    for (TSNode up = parent(call); !ts_node_is_null(up);
         child = up, up = parent(up))
    {
      const Kind k = kind(up);
      if (k == Kind::FunctionDefinition || k == Kind::LambdaExpression)
        break;
      if (k == Kind::IfStatement && same(child, field(up, "consequence")) &&
          tests(field(field(up, "condition"), "value"), false, "&&"))
        return true;
      if (k == Kind::ConditionalExpression &&
          same(child, field(up, "consequence")) &&
          tests(field(up, "condition"), false, "&&"))
        return true;
      if (k != Kind::CompoundStatement)
        continue;
      const uint32_t count = namedCount(up);
      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode earlier = namedChild(up, i);
        if (start(earlier) >= start(child))
          break;
        if (kind(earlier) == Kind::IfStatement &&
            ts_node_is_null(field(earlier, "alternative")) &&
            endsInJump(field(earlier, "consequence")) &&
            tests(field(field(earlier, "condition"), "value"), true, "||"))
          return true;
      }
    }
    return false;
  }

  // ---------------------------------------------------------- has-then-get

  // The jump an early-out ends in: a return, continue or break, alone or last
  // in a block.
  bool endsInJump(TSNode statement) const
  {
    if (kind(statement) == Kind::CompoundStatement)
    {
      const uint32_t count = namedCount(statement);
      if (count == 0)
        return false;
      TSNode last = namedChild(statement, count - 1);
      for (uint32_t i = count; i > 0 && kind(last) == Kind::Comment; --i)
        last = namedChild(statement, i - 1);
      statement = last;
    }
    const Kind k = kind(statement);
    return k == Kind::ReturnStatement || k == Kind::ContinueStatement ||
           k == Kind::BreakStatement;
  }

  // Whether `node` is `E.isValid()` (or `!E.isValid()` with `negated`) for the
  // handle a lookup is on.
  bool validTest(TSNode node, const Lookup& has, bool negated) const
  {
    node = unparenthesised(node);
    if (negated)
    {
      if (!isNot(node))
        return false;
      node = unparenthesised(field(node, "argument"));
    }
    TSNode templateArguments;
    if (kind(node) != Kind::CallExpression ||
        f.calleeName(node, templateArguments) != "isValid")
      return false;
    if (ts_node_is_null(has.registry))
      return namedCount(field(node, "arguments")) == 0 &&
             sameSpelling(text(f.receiverOf(node)), text(has.entity));
    const TSNode arguments = field(node, "arguments");
    return namedCount(arguments) == 1 &&
           sameSpelling(text(namedChild(arguments, 0)), text(has.entity));
  }

  // Whether `operand` is an operand of the && chain `top` itself, not of a
  // parenthesised part of it.
  bool flatChain(TSNode operand, TSNode top) const
  {
    const TSNode chain = unparenthesised(top);
    for (TSNode up = parent(operand); !ts_node_is_null(up) && !same(up, chain);
         up = parent(up))
      if (!isBinary(up, "&&"))
        return false;
    return true;
  }

  // What a has-test guards and how its fix is written.
  enum class Shape : uint8_t
  {
    None,
    Ternary,  // (a) E.has<T>() ? &E.get<T>() : nullptr
    If,       // (b) if (E.has<T>()) S
    IfAnd,    // (c) if (E.has<T>() && REST) S
    EarlyOut, // (d) if (!E.has<T>()) return; then the rest of the block
    Reported, // guarded, but no fix shape
  };

  void hasThenGet(TSNode call)
  {
    Lookup has;
    if (!lookupOf(call, has) || inRemovalHook(call))
      return;
    TSNode test = call;
    const bool negated = isNot(parent(call));
    if (negated)
      test = parent(call);
    TSNode outer = test;
    TSNode up = outerParent(outer);

    // The code that runs only when the test is true, and the shape of the fix.
    std::vector<TSNode> regions;
    std::vector<TSNode> chain; // the condition's && or || operands
    Shape shape = Shape::None;
    TSNode statement{}; // the if, or the ternary for the Ternary shape
    TSNode condition{}; // the whole condition expression
    if (!negated)
    {
      // Climb a && chain the test is an operand of.
      TSNode top = outer;
      TSNode above = up;
      while (isBinary(above, "&&"))
      {
        top = above;
        TSNode climb = top;
        above = outerParent(climb);
        top = climb;
      }
      if (!same(top, outer))
      {
        operands(top, "&&", chain);
        bool after = false;
        for (const TSNode operand : chain)
        {
          if (after)
            regions.push_back(operand);
          after = after || within(outer, operand);
        }
      }
      if (kind(above) == Kind::ConditionClause &&
          same(field(above, "value"), top))
      {
        statement = parent(above);
        condition = top;
        if (kind(statement) == Kind::IfStatement)
        {
          regions.push_back(field(statement, "consequence"));
          const bool init = !ts_node_is_null(field(above, "initializer"));
          if (init)
            shape = Shape::Reported;
          else if (same(top, outer))
            shape = Shape::If;
          else if ((within(outer, chain[0]) ||
                    (chain.size() > 1 && validTest(chain[0], has, false) &&
                     within(outer, chain[1]))) &&
                   flatChain(outer, top))
            shape = Shape::IfAnd;
          else
            shape = Shape::Reported;
        }
        else if (kind(statement) == Kind::WhileStatement)
        {
          regions.push_back(field(statement, "body"));
          shape = Shape::Reported;
        }
      }
      else if (kind(above) == Kind::ConditionalExpression &&
               same(field(above, "condition"), top))
      {
        regions.push_back(field(above, "consequence"));
        shape = Shape::Reported;
        const TSNode consequence = unparenthesised(field(above, "consequence"));
        const TSNode alternative = unparenthesised(field(above, "alternative"));
        TSNode got = consequence;
        if (kind(got) == Kind::PointerExpression &&
            text(field(got, "operator")) == "&")
          got = field(got, "argument");
        // The test alone, or after the handle's validity test (a null
        // lookup says both).
        const bool alone =
            same(top, outer) ||
            (chain.size() == 2 && validTest(chain[0], has, false) &&
             within(outer, chain[1]) && flatChain(outer, top));
        Lookup lookup;
        if (alone && trimmed(text(alternative)) == "nullptr" &&
            kind(got) == Kind::CallExpression && lookupOf(got, lookup) &&
            text(lookup.name) == "getComponent" && sameLookup(has, lookup))
        {
          statement = above;
          shape = Shape::Ternary;
        }
      }
      else if (!regions.empty())
        shape = Shape::Reported;
    }
    else
    {
      // Climb a || chain: `if (!E.isValid() || !E.has<T>()) return;`.
      TSNode top = outer;
      TSNode above = up;
      while (isBinary(above, "||"))
      {
        TSNode climb = above;
        above = outerParent(climb);
        top = climb;
      }
      if (!same(top, outer))
      {
        operands(top, "||", chain);
        bool after = false;
        for (const TSNode operand : chain)
        {
          if (after)
            regions.push_back(operand);
          after = after || within(outer, operand);
        }
      }
      if (kind(above) == Kind::ConditionClause &&
          same(field(above, "value"), top) &&
          kind(parent(above)) == Kind::IfStatement)
      {
        statement = parent(above);
        condition = top;
        const TSNode alternative = field(statement, "alternative");
        if (!ts_node_is_null(alternative))
        {
          regions.push_back(alternative);
          shape = Shape::Reported;
        }
        else if (endsInJump(field(statement, "consequence")))
        {
          const TSNode block = parent(statement);
          if (kind(block) == Kind::CompoundStatement)
          {
            const uint32_t count = namedCount(block);
            for (uint32_t i = 0; i < count; ++i)
            {
              const TSNode later = namedChild(block, i);
              if (start(later) >= end(statement))
                regions.push_back(later);
            }
          }
          shape = ts_node_is_null(field(above, "initializer")) &&
                          kind(block) == Kind::CompoundStatement
                      ? Shape::EarlyOut
                      : Shape::Reported;
        }
      }
      else if (!regions.empty())
        shape = Shape::Reported;
    }
    if (shape == Shape::None || regions.empty())
      return;

    // The lookups the test guards, on a handle nothing changes in between.
    const TSNode scope = functionOf(call);
    std::vector<TSNode> guarded;
    for (const TSNode lookupCall : f.lookups)
    {
      Lookup lookup;
      if (start(lookupCall) < end(call) || !lookupOf(lookupCall, lookup) ||
          !sameLookup(has, lookup))
        continue;
      bool inside = false;
      for (const TSNode region : regions)
        inside = inside || (within(lookupCall, region) &&
                            !inLambdaWithin(lookupCall, region));
      if (!inside ||
          (!ts_node_is_null(scope) &&
           changedBetween(has.entity, scope, end(call), start(lookupCall))))
        continue;
      guarded.push_back(lookupCall);
    }
    if (guarded.empty())
      return;

    const std::string type = tidy(text(has.type));
    const std::string message =
        "This looks " + type + " up twice, with hasComponent<" + type +
        ">() and then getComponent<" + type + ">(). Use getComponent<" + type +
        ">() alone, which gives null when " + type + " is missing.";
    std::vector<TextEdit> fix;
    std::string title;
    if (shape == Shape::Ternary)
    {
      fix.push_back(replace(statement, getText(has)));
      title = "Use getComponent<" + type + ">() alone";
    }
    else if (shape != Shape::Reported &&
             fixGuarded(
                 has, shape, statement, condition, outer, chain, guarded, fix))
      title = "Look " + type + " up once";
    if (fix.empty())
      f.report(RuleId::HasThenGet, call, message);
    else
      f.report(
          RuleId::HasThenGet, call, message, std::move(title), std::move(fix));
  }

  // The fix of the If, IfAnd and EarlyOut shapes: one lookup into a pointer,
  // the guarded lookups read through it. False when it can't be written.
  bool fixGuarded(const Lookup& has,
                  Shape shape,
                  TSNode statement,
                  TSNode condition,
                  TSNode test,
                  const std::vector<TSNode>& chain,
                  const std::vector<TSNode>& guarded,
                  std::vector<TextEdit>& fix)
  {
    // A guarded `T& x = E.get<T>();` lends its name; else one from T.
    TSNode bound{};
    for (const TSNode lookup : guarded)
    {
      bound = boundDeclaration(lookup);
      if (!ts_node_is_null(bound))
        break;
    }
    std::string name;
    TSNode boundScope{};
    if (!ts_node_is_null(bound))
    {
      name = std::string(f.declaredName(field(bound, "declarator")));
      boundScope = scopeOfStatement(bound);
    }
    else
      name = nameFor(text(has.type));
    if (name.empty())
      return false;

    // Where the new name is seen: the if, or for an early-out the rest of
    // the block. It mustn't already mean something there.
    const TSNode block = parent(statement);
    const TSNode reach = shape == Shape::EarlyOut ? block : statement;
    const uint32_t from = start(statement);
    if (!ts_node_is_null(declarationOf(statement, name)))
      return false;
    if (shape == Shape::EarlyOut)
    {
      // Declared earlier in the block, or as the function's parameter.
      const uint32_t count = namedCount(block);
      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode earlier = namedChild(block, i);
        if (start(earlier) >= from)
          break;
        if (declares(earlier, name))
          return false;
      }
    }
    if (ts_node_is_null(bound))
    {
      if (mentions(reach, name, TSNode{}, from))
        return false;
    }
    else if ((!same(boundScope, reach) &&
              mentions(reach, name, boundScope, from)) ||
             mentionsBetween(boundScope,
                             name,
                             std::max(from, start(boundScope)),
                             start(bound)))
      // The bound name's own uses are fine; anything else of that name, in
      // the reach or before the declaration, is something else.
      return false;

    const bool constant = !ts_node_is_null(bound) && constType(bound);
    const std::string declared = (constant ? "const " : "") +
                                 tidy(text(has.type)) + "* " + name + " = " +
                                 getText(has);
    switch (shape)
    {
    case Shape::If:
      fix.push_back(replace(condition, declared));
      break;
    case Shape::IfAnd:
      // The declaration can't be in parentheses: theirs go.
      for (TSNode wrap = condition; kind(wrap) == Kind::ParenthesizedExpression;
           wrap = namedChild(wrap, 0))
      {
        fix.push_back(fileLines().edit(start(wrap), start(wrap) + 1, {}));
        fix.push_back(fileLines().edit(end(wrap) - 1, end(wrap), {}));
      }
      // With nothing after the test, the declaration is the condition.
      fix.push_back(fileLines().edit(
          start(chain[0]),
          end(test),
          within(test, chain.back()) ? declared : declared + "; " + name));
      break;
    case Shape::EarlyOut:
    {
      // The test first, or after E's validity test (dropped): the lookup
      // mustn't run ahead of a term that used to cut the condition short.
      std::size_t index = 0;
      while (index < chain.size() && !within(test, chain[index]))
        ++index;
      if (chain.empty())
        index = 0;
      if (index > 1 || (index == 1 && !validTest(chain[0], has, true)))
        return false;
      if (index == 1)
        fix.push_back(fileLines().edit(start(chain[0]), start(chain[1]), {}));
      const Lines& source = fileLines();
      fix.push_back(source.edit(
          from, from, declared + ";\n" + std::string(source.indentOf(from))));
      fix.push_back(replace(test, "!" + name));
      break;
    }
    default:
      return false;
    }

    for (const TSNode lookup : guarded)
    {
      if (!ts_node_is_null(bound) && within(lookup, bound))
      {
        fix.push_back(removeStatement(bound));
        if (!pointerUses(boundScope, end(bound), name, fix))
          return fix.clear(), false;
        continue;
      }
      readThrough(lookup, name, true, fix);
    }
    return true;
  }

  // Whether `scope` mentions `name` between the bytes `from` and `to`.
  bool mentionsBetween(TSNode scope,
                       std::string_view name,
                       uint32_t from,
                       uint32_t to)
  {
    bool found = false;
    walkTree(scope,
             [&](TSNode node)
             {
               if (start(node) >= to || end(node) <= from)
                 return Next::Skip;
               found = kind(node) == Kind::Identifier && start(node) >= from &&
                       end(node) <= to && text(node) == name;
               return found ? Next::Stop : Next::Descend;
             });
    return found;
  }

  // ------------------------------------------------------- component-pointer

  void componentPointer(TSNode call)
  {
    Lookup lookup;
    if (!lookupOf(call, lookup) && !plainLookup(call, lookup))
      return;
    const std::string type = tidy(text(lookup.type));
    const std::string message =
        "This uses getComponent<" + type +
        ">() as a reference, but it now gives a pointer, which is null when " +
        type + " is missing. Use -> and check it for null.";
    const TSNode up = parent(call);
    if (kind(up) == Kind::FieldExpression && same(field(up, "argument"), call))
    {
      const TSNode op = operatorOf(up);
      if (text(op) != ".")
        return;
      f.report(RuleId::ComponentPointer,
               call,
               message,
               "Use ->",
               {replace(op, "->")});
      return;
    }
    if (kind(up) == Kind::PointerExpression &&
        text(field(up, "operator")) == "&")
    {
      f.report(RuleId::ComponentPointer,
               call,
               message,
               "Drop the &",
               {fileLines().edit(start(up), start(call), {})});
      return;
    }
    if (kind(up) == Kind::AssignmentExpression && same(field(up, "left"), call))
    {
      // The whole component assigned: `E.getComponent<T>() = v`.
      f.report(RuleId::ComponentPointer,
               call,
               message,
               "Assign through *",
               {fileLines().edit(start(call), start(call), "*")});
      return;
    }
    if (kind(up) == Kind::ArgumentList)
    {
      // Passed straight on: reported where the callee is seen to take T&, or
      // to have a pointer-taking overload beside a T& one, which a pointer
      // would quietly pick (a pool slot handed to ::free(void*)).
      if (const std::string callee = passedTo(up, call, lookup.type);
          !callee.empty())
        f.report(RuleId::ComponentPointer, call, callee);
      return;
    }
    if (kind(up) != Kind::InitDeclarator || !same(field(up, "value"), call))
      return;
    const TSNode declarator = field(up, "declarator");
    const TSNode declaration = parent(up);
    if (kind(declarator) == Kind::ReferenceDeclarator)
    {
      const TSNode bound = boundDeclaration(call);
      std::vector<TextEdit> fix;
      if (!ts_node_is_null(bound) && !f.declaredName(declarator).empty())
      {
        const std::string_view name = f.declaredName(declarator);
        fix.push_back(
            fileLines().edit(start(declarator), start(declarator) + 1, "*"));
        const TSNode scope = scopeOfStatement(bound);
        if (kind(scope) == Kind::TranslationUnit ||
            kind(scope) == Kind::FieldDeclarationList ||
            !pointerUses(scope, end(bound), name, fix))
          fix.clear();
      }
      if (fix.empty())
        f.report(RuleId::ComponentPointer, call, message);
      else
        f.report(
            RuleId::ComponentPointer,
            call,
            message,
            "Make " + std::string(f.declaredName(declarator)) + " a pointer",
            std::move(fix));
      return;
    }
    // A copy: `T x = E.getComponent<T>()`.
    if (kind(declarator) == Kind::Identifier &&
        kind(declaration) == Kind::Declaration &&
        sameSpelling(text(typeOf(declaration)), text(lookup.type)))
      f.report(RuleId::ComponentPointer,
               call,
               message,
               "Copy through *",
               {fileLines().edit(start(call), start(call), "*")});
  }

  // How a parameter takes an argument, for passedTo().
  enum class Takes : uint8_t
  {
    Reference, // T& or const T&, T the lookup's component
    Pointer,   // any pointer (T*, void*, a base's)
    Other,     // another type, by value or reference
    Generic,   // a template parameter, auto, or past a `...`
  };

  // A function declaration the rules can see: its declarator, and the text
  // it's written in.
  struct Declared
  {
    std::string_view name;
    TSNode declarator; // the function_declarator
    std::string_view source;
  };
  std::optional<std::vector<Declared>> declaredFunctions;

  // The functions declared in the linted file (whose tree `at` is in) and
  // the files it includes ("..."), once a run.
  const std::vector<Declared>& functions(TSNode at)
  {
    if (declaredFunctions)
      return *declaredFunctions;
    declaredFunctions.emplace();
    const auto collect = [&](TSNode root, std::string_view source)
    {
      walkTree(root,
               [&](TSNode node)
               {
                 if (kind(node) != Kind::FunctionDeclarator)
                   return Next::Descend;
                 TSNode name = field(node, "declarator");
                 while (kind(name) == Kind::QualifiedIdentifier)
                   name = field(name, "name");
                 if (kind(name) == Kind::Identifier ||
                     kind(name) == Kind::FieldIdentifier)
                   declaredFunctions->push_back(
                       {codegen::nodeText(source, name), node, source});
                 return Next::Descend;
               });
    };
    TSNode root = at;
    while (!ts_node_is_null(parent(root)))
      root = parent(root);
    collect(root, f.source);
    for (const std::filesystem::path& path : includes())
      if (const ReadFile* file = readFile(path))
        collect(ts_tree_root_node(file->tree.get()), file->text);
    return *declaredFunctions;
  }

  // How a declared function takes argument `index` of `count`, for a lookup
  // of `component`; nullopt when it can't take that many arguments.
  std::optional<Takes> takes(const Declared& function,
                             std::size_t index,
                             std::size_t count,
                             std::string_view component) const
  {
    const TSNode parameters = field(function.declarator, "parameters");
    std::vector<TSNode> list;
    std::size_t required = 0;
    bool variadic = false;
    const uint32_t children = childCountOf(parameters);
    for (uint32_t i = 0; i < children; ++i)
    {
      const TSNode child = nthChild(parameters, i);
      const std::string_view type = typeName(child);
      if (type == "parameter_declaration")
      {
        list.push_back(child);
        ++required;
      }
      else if (type == "optional_parameter_declaration")
        list.push_back(child);
      else if (type == "variadic_parameter_declaration" ||
               codegen::nodeText(function.source, child) == "...")
        variadic = true;
    }
    // (void) takes nothing.
    if (list.size() == 1 && required == 1 &&
        trimmed(codegen::nodeText(function.source, list[0])) == "void")
      list.clear(), required = 0;
    if (count < required || (count > list.size() && !variadic))
      return std::nullopt;
    if (index >= list.size())
      return Takes::Generic;
    const TSNode parameter = list[index];
    const TSNode type = field(parameter, "type");
    const std::string_view typeText = codegen::nodeText(function.source, type);
    const auto plain = [](std::string_view spelled)
    {
      if (const std::size_t open = spelled.find('<');
          open != std::string_view::npos)
        spelled = spelled.substr(0, open);
      return lastName(spelled);
    };
    if (std::strcmp(typeName(type), "placeholder_type_specifier") == 0)
      return Takes::Generic;
    // A template parameter of the function's template.
    for (TSNode up = parent(function.declarator); !ts_node_is_null(up);
         up = parent(up))
    {
      if (kind(up) != Kind::TemplateDeclaration)
        continue;
      const TSNode templateParameters = field(up, "parameters");
      const uint32_t names = namedCount(templateParameters);
      for (uint32_t i = 0; i < names; ++i)
      {
        std::string_view declared = trimmed(codegen::nodeText(
            function.source, namedChild(templateParameters, i)));
        if (const std::size_t space = declared.find_last_of(" .");
            space != std::string_view::npos)
          declared = declared.substr(space + 1);
        if (declared == plain(typeText))
          return Takes::Generic;
      }
    }
    const TSNode declarator = field(parameter, "declarator");
    if (kind(declarator) == Kind::PointerDeclarator)
      return Takes::Pointer;
    if (kind(declarator) == Kind::ReferenceDeclarator &&
        !codegen::nodeText(function.source, declarator).starts_with("&&") &&
        plain(typeText) == plain(component))
      return Takes::Reference;
    return Takes::Other;
  }

  /*
   * For a lookup passed as an argument: the warning to give when the callee,
   * found by name and argument count among the functions the rules can see,
   * takes T& there, or has a pointer-taking overload beside a T& one. Empty
   * (no warning) when it takes T*, can't be found, or may be generic, and for
   * sfs::expect, which takes a lookup to check it.
   */
  std::string passedTo(TSNode arguments, TSNode call, TSNode component)
  {
    if (expectArgument(arguments))
      return {};
    const TSNode caller = parent(arguments);
    if (kind(caller) != Kind::CallExpression)
      return {};
    TSNode function = field(caller, "function");
    if (kind(function) == Kind::FieldExpression)
      function = field(function, "field");
    std::string_view name = text(function);
    if (const std::size_t open = name.find('<'); open != std::string_view::npos)
      name = name.substr(0, open);
    name = lastName(name);
    const std::size_t count = namedCount(arguments);
    std::size_t index = 0;
    while (index < count && !same(namedChild(arguments, index), call))
      ++index;
    bool reference = false;
    bool pointer = false;
    for (const Declared& declared : functions(call))
    {
      if (declared.name != name)
        continue;
      const std::optional<Takes> how =
          takes(declared, index, count, text(component));
      if (!how)
        continue;
      if (*how == Takes::Generic)
        return {};
      reference = reference || *how == Takes::Reference;
      pointer = pointer || *how == Takes::Pointer;
    }
    if (!reference)
      return {};
    const std::string type = tidy(text(component));
    const std::string callee(name);
    return "This passes getComponent<" + type + ">() to " + callee +
           "(), which takes " + type + "& here" +
           (pointer ? " beside an overload taking a pointer that the lookup "
                      "would quietly pick"
                    : "") +
           ", but the lookup now gives a pointer. Check it for null, then "
           "pass what it points to.";
  }

  // Whether an argument list is sfs::expect()'s, which takes the lookup to
  // check it.
  bool expectArgument(TSNode arguments) const
  {
    const TSNode call = parent(arguments);
    if (kind(call) != Kind::CallExpression)
      return false;
    std::string_view callee = text(field(call, "function"));
    if (const std::size_t open = callee.find('<');
        open != std::string_view::npos)
      callee = callee.substr(0, open);
    return lastName(callee) == "expect";
  }

  // A lookup on any receiver (a call result, `*p`), for the forms that don't
  // need the handle: `.f`, `&`, a reference or a copy.
  bool plainLookup(TSNode call, Lookup& out) const
  {
    TSNode templateArguments;
    if (f.calleeName(call, templateArguments) != "getComponent" ||
        namedCount(templateArguments) != 1)
      return false;
    out.call = call;
    out.type = namedChild(templateArguments, 0);
    out.entity = TSNode{};
    out.registry = TSNode{};
    return true;
  }

  // ------------------------------------------------------------ row-lookup

  // A lookup in a loop, and the bound term it reads.
  struct RowUse
  {
    TSNode call;
    std::size_t term;
  };

  // One component a query row binds, in order.
  struct Bound
  {
    std::string_view spelling;
    bool any;               // an Any member: bound as a pointer
    std::string_view group; // its Any group's spelling, for an Any member
    // A term of a typed system list's registration that the list's own terms
    // leave out: its rows have it, but bind it only if it's added.
    bool implied = false;
  };

  // What a range-for goes through, read as a query's row.
  struct Row
  {
    TSNode range{};    // the call: query, getEntities, a walk or entities()
    TSNode terms{};    // its template argument list, if it has one
    bool inPlace{};    // the call is the loop's range, not a variable's or
                       // a helper's
    bool untyped{};    // a system's getEntities(), its terms its registrations
    bool everything{}; // the registry's entities(): no terms
    bool walk{};       // a typed hierarchy walk: a query's rows
    // A typed system list whose system's registration couldn't be read: its
    // rows may have more than its terms say.
    bool unknownTerms{};
    std::vector<Bound> bound;
    std::vector<std::string_view> excluded; // Excluding<...> members
  };
  // The row of the loop being checked, a system's registered terms and what
  // they bind, kept from loop to loop.
  Row m_row;
  std::vector<std::string_view> m_registered;
  std::vector<Bound> m_registeredBound;
  std::vector<std::string_view> m_excludedScratch;
  // filter-in-loop's scratch: a loop body's statements, a condition's
  // operands, the lookups a body opens with and their names, and the
  // lookups the loop needs to go on.
  std::vector<TSNode> m_statements;
  std::vector<TSNode> m_terms;
  std::vector<Lookup> m_declared;
  std::vector<std::string_view> m_declaredNames;
  std::vector<Lookup> m_needed;

  // The components a term binds (required, Any members; Excluding binds
  // nothing, its members going to `excluded`; aliases and Query lists in
  // order). False when it can't be read.
  bool boundTerms(TSNode at,
                  std::string_view owner,
                  std::string_view term,
                  std::vector<Bound>& out,
                  std::vector<std::string_view>& excluded,
                  int depth = 0)
  {
    if (depth > 8 || term.ends_with("..."))
      return false;
    std::string_view inner;
    switch (f.wrapperOfText(at, term, inner))
    {
    case Wrapper::None:
      if (const QueryAlias* named = f.alias(owner, trimmed(term)))
      {
        for (const std::string& nested : named->terms)
          if (!boundTerms(at, owner, nested, out, excluded, depth + 1))
            return false;
        return true;
      }
      out.push_back({trimmed(term), false, {}});
      return true;
    case Wrapper::Query:
      for (const std::string_view nested : splitTerms(inner))
        if (!boundTerms(at, owner, nested, out, excluded, depth + 1))
          return false;
      return true;
    case Wrapper::Any:
      for (const std::string_view member : splitTerms(inner))
        out.push_back({member, true, trimmed(term)});
      return true;
    case Wrapper::Excluding:
      for (const std::string_view member : splitTerms(inner))
        excluded.push_back(member);
      return true;
    }
    return true;
  }

  // The template arguments of a query<...>() or getEntities<...>() call as
  // the row's bound and excluded components.
  bool
  boundOf(TSNode templateArguments, TSNode at, std::string_view owner, Row& row)
  {
    const uint32_t count = namedCount(templateArguments);
    for (uint32_t i = 0; i < count; ++i)
      if (!boundTerms(at,
                      owner,
                      text(namedChild(templateArguments, i)),
                      row.bound,
                      row.excluded))
        return false;
    return true;
  }

  // Whether a class body (written in `source`) declares or defines a member
  // function called `name`.
  bool hasMember(TSNode classNode,
                 std::string_view source,
                 std::string_view name) const
  {
    const TSNode body = field(classNode, "body");
    const uint32_t count = ts_node_is_null(body) ? 0 : namedCount(body);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode member = namedChild(body, i);
      const TSNode declared = memberName(member);
      if (!ts_node_is_null(declared) &&
          codegen::nodeText(source, declared) == name)
        return true;
    }
    return false;
  }

  // The function definitions at the top level of the linted file, once a run.
  const std::vector<TSNode>& ownDefinitions(TSNode at)
  {
    if (!fileDefinitions)
      fileDefinitions = definitionsIn(at);
    return *fileDefinitions;
  }
  std::optional<std::vector<TSNode>> fileDefinitions;

  /*
   * The query a helper call returns: `helper()` (unqualified or through
   * `this`) naming one function of this file without parameters, a member of
   * the loop's class `owner` or a free function the class doesn't hide, that
   * returns `const std::vector<sfs::Entity>&` and whose body is only
   * `return <query<...>() or getEntities(...)>;`. Null when it's anything
   * else, a filtered copy included.
   */
  TSNode helperQuery(TSNode call, std::string_view owner)
  {
    TSNode function = field(call, "function");
    if (kind(function) == Kind::FieldExpression &&
        kind(field(function, "argument")) == Kind::This)
      function = field(function, "field");
    if (kind(function) != Kind::Identifier &&
        kind(function) != Kind::FieldIdentifier)
      return {};
    const std::string_view name = text(function);

    // The definitions it may name: members first, then free functions.
    TSNode member{}, free{};
    int members = 0, frees = 0;
    const FileClass* fileClass = f.fileClass(owner);
    if (fileClass)
    {
      const TSNode body = field(fileClass->node, "body");
      for (uint32_t i = 0; i < namedCount(body); ++i)
      {
        const TSNode child = namedChild(body, i);
        if (kind(child) == Kind::FunctionDefinition &&
            text(memberName(child)) == name)
          member = child, ++members;
      }
    }
    for (const TSNode definition : ownDefinitions(call))
    {
      const TSNode declared = memberName(definition);
      if (kind(declared) == Kind::QualifiedIdentifier)
      {
        if (!owner.empty() && text(field(declared, "name")) == name &&
            lastName(text(field(declared, "scope"))) == owner)
          member = definition, ++members;
      }
      else if (kind(declared) == Kind::Identifier && text(declared) == name)
        free = definition, ++frees;
    }
    TSNode definition = member;
    if (members == 0 && frees == 1)
    {
      // A free function, unless the loop's class may have a member by that
      // name that the call would mean instead.
      if (!owner.empty())
      {
        const ClassHeader* header = fileClass ? nullptr : classHeader(owner);
        if (fileClass
                ? hasMember(fileClass->node, f.source, name)
                : !header || hasMember(header->node, header->file->text, name))
          return {};
      }
      definition = free;
    }
    else if (members != 1)
      return {};

    // No parameters, a const vector of entities by reference, one return.
    const TSNode parameters = parametersOf(definition);
    const TSNode reference = field(definition, "declarator");
    const std::string type = tidy(text(typeOf(definition)));
    if (namedCount(parameters) != 0 ||
        kind(reference) != Kind::ReferenceDeclarator ||
        text(reference).starts_with("&&") || !constType(definition) ||
        !(type == "std::vector<sfs::Entity>" ||
          (type == "std::vector<Entity>" &&
           f.isSfsNameAt(definition, "Entity", "Entity"))))
      return {};
    // `= default` and `= delete` definitions have no body.
    const TSNode body = field(definition, "body");
    const TSNode only = soleStatement(body);
    if (kind(body) != Kind::CompoundStatement ||
        kind(only) != Kind::ReturnStatement || namedCount(only) != 1)
      return {};
    const TSNode returned = unparenthesised(namedChild(only, 0));
    TSNode templateArguments;
    const std::string_view callee =
        kind(returned) == Kind::CallExpression
            ? f.calleeName(returned, templateArguments)
            : std::string_view{};
    if ((callee != "query" && callee != "getEntities") ||
        namedCount(field(returned, "arguments")) != 0)
      return {};
    return returned;
  }

  // Whether `entities()` is called on the registry or the scene: a name or
  // field chain whose last name ends in "registry" or "scene", whatever the
  // case (`registry`, `m_registry`, `context.scene`), or `scene()`. A query's
  // view has an entities() too, which is its own list.
  bool registryReceiver(TSNode receiver) const
  {
    if (kind(receiver) == Kind::CallExpression)
    {
      TSNode templateArguments;
      const TSNode inner = f.receiverOf(receiver);
      return f.calleeName(receiver, templateArguments) == "scene" &&
             ts_node_is_null(templateArguments) &&
             namedCount(field(receiver, "arguments")) == 0 &&
             (ts_node_is_null(inner) || kind(inner) == Kind::This);
    }
    if (!plainChain(receiver) || kind(receiver) == Kind::This)
      return false;
    const TSNode last = kind(receiver) == Kind::FieldExpression
                            ? field(receiver, "field")
                            : receiver;
    std::string lower(text(last));
    for (char& c : lower)
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c - 'A' + 'a');
    return lower.ends_with("registry") || lower.ends_with("scene");
  }

  // The terms of a typed hierarchy walk, `sfs::hierarchy::childrenOf<Terms>(e)`
  // or `descendants<Terms>(e)` (written with `hierarchy::`), whose rows are
  // a query's; null for any other call.
  TSNode walkTerms(TSNode call) const
  {
    const TSNode function = field(call, "function");
    TSNode name = function;
    while (kind(name) == Kind::QualifiedIdentifier)
      name = field(name, "name");
    if (kind(function) != Kind::QualifiedIdentifier ||
        kind(name) != Kind::TemplateFunction ||
        namedCount(field(call, "arguments")) != 1)
      return {};
    const std::string_view walk = text(field(name, "name"));
    const std::string_view scope =
        f.source.substr(start(function), start(name) - start(function));
    if ((walk != "childrenOf" && walk != "descendants") ||
        !tidy(scope).ends_with("hierarchy::"))
      return {};
    return field(name, "arguments");
  }

  // The row a range-for goes through: a query<Terms>() (any receiver), a
  // getEntities<Terms>(), a system's own getEntities() with its registered
  // terms known, a typed hierarchy walk (walkTerms()), a helper returning a
  // query or a system's list (helperQuery()), or a variable bound to any of
  // them in the same function; or every entity, for the registry's
  // entities(). False for anything else, or terms it can't read.
  bool rowOf(TSNode loop, Row& row)
  {
    row.bound.clear();
    row.excluded.clear();
    row.terms = TSNode{};
    row.inPlace = true;
    row.untyped = false;
    row.everything = false;
    row.walk = false;
    row.unknownTerms = false;
    TSNode range = field(loop, "right");
    if (kind(range) == Kind::Identifier)
    {
      range = boundList(loop, range);
      row.inPlace = false;
    }
    if (kind(range) != Kind::CallExpression)
      return false;
    const std::string_view owner = f.ownerAt(loop);
    if (const TSNode terms = walkTerms(range); !ts_node_is_null(terms))
    {
      row.range = range;
      row.terms = terms;
      row.walk = true;
      return boundOf(terms, range, owner, row);
    }
    if (namedCount(field(range, "arguments")) != 0)
      return false;
    TSNode templateArguments;
    std::string_view callee = f.calleeName(range, templateArguments);
    if (callee == "entities" && ts_node_is_null(templateArguments))
    {
      row.range = range;
      row.everything = registryReceiver(f.receiverOf(range));
      return row.everything;
    }
    if (callee != "query" && callee != "getEntities")
    {
      range = helperQuery(range, owner);
      if (ts_node_is_null(range))
        return false;
      row.inPlace = false;
      callee = f.calleeName(range, templateArguments);
    }
    row.range = range;
    row.terms = templateArguments;
    if (callee == "query" && !ts_node_is_null(templateArguments))
      return boundOf(templateArguments, range, owner, row);
    if (callee != "getEntities")
      return false;
    const TSNode receiver = f.receiverOf(range);
    if (!ts_node_is_null(receiver) && kind(receiver) != Kind::This)
      return false;
    m_registered.clear();
    const bool registered = registeredOf(owner, range, m_registered);
    if (!ts_node_is_null(templateArguments))
    {
      // A typed system list: its rows also have every registered term.
      if (!boundOf(templateArguments, range, owner, row))
        return false;
      m_registeredBound.clear();
      bool read = registered;
      for (const std::string_view term : m_registered)
        read = read &&
               boundTerms(range, owner, term, m_registeredBound, row.excluded);
      row.unknownTerms = !read;
      if (read)
        for (Bound& extra : m_registeredBound)
          if (std::none_of(row.bound.begin(),
                           row.bound.end(),
                           [&](const Bound& known) {
                             return sameType(known.spelling, extra.spelling);
                           }))
          {
            extra.implied = true;
            row.bound.push_back(extra);
          }
      return true;
    }
    if (!registered)
      return false;
    for (const std::string_view term : m_registered)
      if (!boundTerms(range, owner, term, row.bound, row.excluded))
        return false;
    row.untyped = true;
    return true;
  }

  // Whether two spellings name one type: the same, or with the same last
  // name (`sfs::Hitch` and `Hitch`), a leading `const` aside.
  static bool sameType(std::string_view a, std::string_view b)
  {
    a = unconst(a);
    b = unconst(b);
    return sameSpelling(a, b) || lastName(a) == lastName(b);
  }

  // The call a loop's list variable is bound to, when the list is that call's
  // rows: one variable declared in the loop's function as a const reference
  // or a view (`auto`, `sfs::QueryView<...>`), not a copy, and, if it can be
  // changed, neither assigned, address-taken nor passed on before the loop.
  // Null otherwise.
  TSNode boundList(TSNode loop, TSNode name)
  {
    const TSNode declaration = declarationOf(loop, text(name));
    if (kind(declaration) != Kind::Declaration ||
        !same(functionOf(declaration), functionOf(loop)) ||
        declaratorCount(declaration) != 1)
      return {};
    const TSNode init = field(declaration, "declarator");
    const TSNode declarator = field(init, "declarator");
    const TSNode type = typeOf(declaration);
    const bool constant = constType(declaration);
    const bool reference = kind(declarator) == Kind::ReferenceDeclarator &&
                           !text(declarator).starts_with("&&") && constant;
    const bool view = kind(declarator) == Kind::Identifier && viewType(type);
    if (kind(init) != Kind::InitDeclarator || (!reference && !view))
      return {};
    if (!constant)
    {
      bool changed =
          changedBetween(name, functionOf(loop), end(declaration), start(loop));
      if (!changed)
        walkTree(
            functionOf(loop),
            [&](TSNode node)
            {
              if (end(node) <= end(declaration) || start(node) >= start(loop))
                return Next::Skip;
              const TSNode up = parent(node);
              changed = kind(node) == Kind::Identifier &&
                        text(node) == text(name) &&
                        (kind(up) == Kind::ArgumentList ||
                         (kind(up) == Kind::PointerExpression &&
                          text(field(up, "operator")) == "&"));
              return changed ? Next::Stop : Next::Descend;
            });
      if (changed)
        return {};
    }
    return field(init, "value");
  }

  // Whether a variable's declared type holds a query's view: `auto` or an
  // `sfs::QueryView<...>`, not a vector.
  bool viewType(TSNode type) const
  {
    std::string_view spelled = trimmed(text(type));
    if (const std::size_t open = spelled.find('<');
        open != std::string_view::npos)
      spelled = spelled.substr(0, open);
    return std::strcmp(typeName(type), "placeholder_type_specifier") == 0 ||
           lastName(spelled) == "QueryView";
  }

  // The one statement of a block, comments aside; null when there are more
  // or none, or `block` isn't a block.
  TSNode soleStatement(TSNode block) const
  {
    TSNode only{};
    if (kind(block) != Kind::CompoundStatement)
      return only;
    for (uint32_t i = 0; i < namedCount(block); ++i)
    {
      const TSNode child = namedChild(block, i);
      if (kind(child) == Kind::Comment)
        continue;
      if (!ts_node_is_null(only))
        return {};
      only = child;
    }
    return only;
  }

  // A block's statements, comments aside, into `out`; a statement that
  // isn't a block is its own one.
  void statementsOf(TSNode block, std::vector<TSNode>& out) const
  {
    out.clear();
    if (kind(block) != Kind::CompoundStatement)
    {
      if (!ts_node_is_null(block))
        out.push_back(block);
      return;
    }
    for (uint32_t i = 0; i < namedCount(block); ++i)
      if (kind(namedChild(block, i)) != Kind::Comment)
        out.push_back(namedChild(block, i));
  }

  // How many variables a declaration declares.
  uint32_t declaratorCount(TSNode declaration) const
  {
    uint32_t count = 0;
    for (uint32_t i = 0; i < namedCount(declaration); ++i)
    {
      const char* name = ts_node_field_name_for_named_child(declaration, i);
      count += name && std::strcmp(name, "declarator") == 0;
    }
    return count;
  }

  // The registered terms of system `owner`, in order: the walk's (a system
  // the file declares, or the project's), else those of its constructor and
  // create() read from this file's out-of-line definitions and its class in
  // the header (an engine system split across a header and a .cpp).
  bool registeredOf(std::string_view owner,
                    TSNode at,
                    std::vector<std::string_view>& terms)
  {
    for (const Registration& registration : f.registrations)
      if (registration.system == owner &&
          !ts_node_is_null(registration.argument))
        terms.push_back(text(registration.argument));
    if (terms.empty() && !f.fileClass(owner))
      if (const ClassHeader* header = classHeader(owner);
          header &&
          derivesFromSystem(
              header->node,
              {header->file->text, &header->file->lines, header->file->path}))
        for (const std::string_view method :
             {owner, std::string_view("create")})
        {
          const uint32_t count = namedCount(field(header->node, "body"));
          for (uint32_t i = 0; i < count; ++i)
          {
            const TSNode member = namedChild(field(header->node, "body"), i);
            if (kind(member) == Kind::FunctionDefinition &&
                codegen::nodeText(header->file->text, memberName(member)) ==
                    method)
              registrationsIn(field(member, "body"), header->file->text, terms);
          }
          for (const TSNode function : definitionsIn(at))
          {
            const TSNode name =
                field(field(function, "declarator"), "declarator");
            if (kind(name) == Kind::QualifiedIdentifier &&
                text(field(name, "name")) == method &&
                lastName(text(field(name, "scope"))) == owner)
              registrationsIn(field(function, "body"), f.source, terms);
          }
        }
    if (terms.empty())
      return false;
    for (const std::string_view term : terms)
      if (term.ends_with("..."))
        return false;
    return true;
  }

  // The terms of the registerComponent<T>() calls (unqualified or through
  // `this`) in a body written in `source`, in order.
  void registrationsIn(TSNode body,
                       std::string_view source,
                       std::vector<std::string_view>& terms) const
  {
    walkTree(body,
             [&](TSNode node)
             {
               if (kind(node) != Kind::CallExpression)
                 return Next::Descend;
               TSNode function = field(node, "function");
               if (kind(function) == Kind::FieldExpression &&
                   kind(field(function, "argument")) == Kind::This)
                 function = field(function, "field");
               const Kind k = kind(function);
               if ((k == Kind::TemplateFunction || k == Kind::TemplateMethod) &&
                   codegen::nodeText(source, field(function, "name")) ==
                       "registerComponent")
               {
                 const TSNode arguments = field(function, "arguments");
                 if (namedCount(arguments) == 1)
                   terms.push_back(
                       codegen::nodeText(source, namedChild(arguments, 0)));
               }
               return Next::Descend;
             });
  }

  // Whether a loop's own entity is assigned or has its address taken in the
  // loop, or, held by value (`copy`), used where it may be changed: passed to
  // a call (whose parameter may be a non-const reference) or given a
  // component. A row's entity is a const reference, so neither would compile.
  bool entityTouched(TSNode body, std::string_view entity, bool copy) const
  {
    bool touched = false;
    walkTree(body,
             [&](TSNode node)
             {
               if (kind(node) != Kind::Identifier || text(node) != entity)
                 return Next::Descend;
               const TSNode up = parent(node);
               touched = (kind(up) == Kind::AssignmentExpression &&
                          same(field(up, "left"), node)) ||
                         kind(up) == Kind::UpdateExpression ||
                         (kind(up) == Kind::PointerExpression &&
                          text(field(up, "operator")) == "&");
               if (copy && !touched)
               {
                 TSNode templateArguments;
                 const std::string_view method =
                     kind(up) == Kind::FieldExpression &&
                             same(field(up, "argument"), node)
                         ? f.calleeName(parent(up), templateArguments)
                         : std::string_view{};
                 touched = kind(up) == Kind::ArgumentList ||
                           method == "addComponent" || method == "addTag";
               }
               return touched ? Next::Stop : Next::Descend;
             });
    return touched;
  }

  // row-lookup on a loop over `row` (rowOf()); a loop over a variable bound
  // to the list, or over a helper's, is reported without a fix.
  void rowLookup(TSNode loop, const Row& row)
  {
    const TSNode declarator = field(loop, "declarator");
    const std::string_view entity = f.declaredName(declarator);
    const TSNode body = field(loop, "body");
    if (entity.empty() || ts_node_is_null(body) || row.everything)
      return;
    const std::vector<Bound>& bound = row.bound;

    // The lookups on the loop's own entity of components its row binds.
    std::vector<RowUse> uses;
    bool inLambda = false;
    for (const TSNode call : f.lookups)
    {
      if (!within(call, body))
        continue;
      Lookup lookup;
      if (!lookupOf(call, lookup) || kind(lookup.entity) != Kind::Identifier ||
          text(lookup.entity) != entity ||
          !same(declarationOf(lookup.entity, entity), loop))
        continue;
      for (std::size_t i = 0; i < bound.size(); ++i)
        // A const term's binding stands in only for a const lookup.
        if (sameSpelling(
                unconst(bound[i].spelling), unconst(text(lookup.type))) &&
            (unconst(bound[i].spelling) == trimmed(bound[i].spelling) ||
             unconst(text(lookup.type)) != trimmed(text(lookup.type))))
        {
          uses.push_back({call, i});
          inLambda = inLambda || inLambdaWithin(call, body);
          break;
        }
    }
    if (uses.empty())
      return;

    // Can it be fixed: a list written in the loop, an entity the row can
    // give, no lookup in a lambda?
    std::vector<TextEdit> fix;
    if (row.inPlace && !inLambda && unpackableEntity(loop, entity))
      fixRow(loop, row, entity, uses, fix);
    for (const RowUse& use : uses)
    {
      Lookup lookup;
      lookupOf(use.call, lookup);
      const std::string type = tidy(text(lookup.type));
      const std::string message =
          "This looks " + type +
          " up on the loop's own entity, whose row "
          "already has it. Unpack the row instead, as in for (auto [" +
          std::string(entity) + ", ...] : ...).";
      if (fix.empty())
        f.report(RuleId::RowLookup, use.call, message);
      else
        f.report(RuleId::RowLookup, use.call, message, "Unpack the row", fix);
    }
  }

  // Whether a loop's entity can be a row's: a handle by value or reference
  // (`sfs::Entity` or `auto`), never assigned or address-taken, nor, held by
  // value, passed where it may change (a row's entity is a const reference).
  bool unpackableEntity(TSNode loop, std::string_view entity) const
  {
    const TSNode declarator = field(loop, "declarator");
    const TSNode entityType = typeOf(loop);
    const bool placeholder =
        std::strcmp(typeName(entityType), "placeholder_type_specifier") == 0;
    const bool handle = kind(declarator) == Kind::Identifier ||
                        (kind(declarator) == Kind::ReferenceDeclarator &&
                         !text(declarator).starts_with("&&"));
    return handle &&
           (placeholder || (!ts_node_is_null(entityType) &&
                            f.isSfsNameAt(loop, text(entityType), "Entity"))) &&
           !entityTouched(field(loop, "body"),
                          entity,
                          kind(declarator) == Kind::Identifier);
  }

  // Names the bound terms a row's header binds (`bind`): a name given and
  // `lent` stays, the others are made from their types. False when a made
  // name is taken in the loop, or two names clash.
  bool nameRow(TSNode loop,
               std::string_view entity,
               const std::vector<Bound>& bound,
               const std::vector<bool>& bind,
               const std::vector<bool>& lent,
               std::vector<std::string>& names) const
  {
    for (std::size_t i = 0; i < bound.size(); ++i)
    {
      if (!bind[i])
        continue;
      if (names[i].empty())
        names[i] = nameFor(bound[i].spelling);
      if (names[i].empty() || names[i] == entity)
        return false;
      // A name from a type mustn't mean anything else in the loop.
      if (!lent[i] && mentions(loop, names[i]))
        return false;
      for (std::size_t j = 0; j < i; ++j)
        if (bind[j] && names[j] == names[i])
          return false;
    }
    return true;
  }

  // One component a loop's header binds: its name, its term's spelling, and
  // whether it's an Any member (bound as a pointer).
  struct Binding
  {
    std::string name;
    std::string_view spelling;
    bool any;
  };

  // The structured binding a range-for unpacks its rows into, through a
  // reference declarator (`const auto& [e, a]`); null when it doesn't unpack.
  TSNode unpackingOf(TSNode loop) const
  {
    TSNode declarator = field(loop, "declarator");
    if (kind(declarator) == Kind::ReferenceDeclarator)
      declarator = namedChild(declarator, namedCount(declarator) - 1);
    return kind(declarator) == Kind::StructuredBindingDeclarator ? declarator
                                                                 : TSNode{};
  }

  // Whether `scope` may read `name` outside the nodes `skipped`: any token
  // spelled so (in a lambda, a macro's arguments, sizeof or decltype too),
  // comments and members named alike (`x.name`) aside.
  bool readsName(TSNode scope,
                 std::string_view name,
                 const std::vector<TSNode>& skipped = {}) const
  {
    bool found = false;
    walkTree(scope,
             [&](TSNode node)
             {
               if (kind(node) == Kind::Comment ||
                   std::any_of(skipped.begin(),
                               skipped.end(),
                               [&](TSNode skip) { return same(node, skip); }))
                 return Next::Skip;
               if (namedCount(node) > 0 || !ts_node_is_named(node) ||
                   text(node) != name)
                 return Next::Descend;
               const TSNode up = parent(node);
               found = kind(node) != Kind::FieldIdentifier ||
                       kind(up) != Kind::FieldExpression ||
                       !same(field(up, "field"), node);
               return found ? Next::Stop : Next::Descend;
             });
    return found;
  }

  // Whether `.components()` can follow a loop's range: it's the list's call
  // itself, or a variable holding the call's view (declared `auto` or as a
  // QueryView, not as a vector).
  bool takesComponents(TSNode loop, const Row& row) const
  {
    if (row.inPlace)
      return true;
    const TSNode range = field(loop, "right");
    if (kind(range) != Kind::Identifier)
      return false;
    const TSNode declaration = declarationOf(loop, text(range));
    return same(field(field(declaration, "declarator"), "value"), row.range) &&
           viewType(typeOf(declaration));
  }

  // Where a range-for's loop variable starts: its type, after a C++20
  // init-statement, which stays.
  uint32_t headerStart(TSNode loop) const
  {
    TSNode first = namedChild(loop, 0);
    if (same(first, field(loop, "initializer")))
      first = nextNamed(first);
    return start(first);
  }

  // The edits making a loop's header bind `bound`, its row's components in
  // term order, with `entity` first (`auto [entity, names]`), or, when
  // `entity` is empty, without it: `T& name` for one required component,
  // which the row converts to, else `auto [names]` over the range's
  // `components()`. A loop that unpacks already keeps its qualifiers. False
  // (nothing added) when the components can't be had without the entity.
  bool rowHeader(TSNode loop,
                 const Row& row,
                 std::string_view entity,
                 const std::vector<Binding>& bound,
                 std::vector<TextEdit>& fix)
  {
    const uint32_t from = headerStart(loop);
    const uint32_t to = end(field(loop, "declarator"));
    if (entity.empty() && bound.size() == 1 && !bound[0].any)
    {
      fix.push_back(fileLines().edit(
          from, to, tidy(bound[0].spelling) + "& " + bound[0].name));
      return true;
    }
    if (entity.empty() && !takesComponents(loop, row))
      return false;
    std::string names(entity);
    for (const Binding& binding : bound)
      names += (names.empty() ? "" : ", ") + binding.name;
    if (const TSNode unpacking = unpackingOf(loop); !ts_node_is_null(unpacking))
      fix.push_back(replace(unpacking, "[" + names + "]"));
    else
      fix.push_back(fileLines().edit(from, to, "auto [" + names + "]"));
    if (entity.empty())
    {
      const uint32_t after = end(field(loop, "right"));
      fix.push_back(fileLines().edit(after, after, ".components()"));
    }
    return true;
  }

  // The fix of a loop whose lookups go: its header unpacks the row, the
  // references bound to lookups become bindings, and the other lookups read
  // their binding.
  void fixRow(TSNode loop,
              const Row& row,
              std::string_view entity,
              const std::vector<RowUse>& uses,
              std::vector<TextEdit>& fix)
  {
    // A typed list's row binds every term it lists; the system's own list,
    // and the registered terms a typed system list leaves out, only those
    // used (an Any member's whole group).
    const std::vector<Bound>& bound = row.bound;
    std::vector<bool> bind(bound.size());
    for (std::size_t i = 0; i < bound.size(); ++i)
      bind[i] = !row.untyped && !bound[i].implied;
    for (const RowUse& use : uses)
    {
      bind[use.term] = true;
      if (bound[use.term].any)
        for (std::size_t i = 0; i < bound.size(); ++i)
          bind[i] = bind[i] ||
                    (bound[i].any && bound[i].group == bound[use.term].group);
    }

    // Names: a reference bound to a lookup (`T& n = v.getComponent<T>()`) in
    // a block or an if's init-statement lends its own, when nothing else in
    // the loop goes by it; the declaration goes.
    std::vector<std::string> names(bound.size());
    std::vector<bool> lent(bound.size(), false);
    std::vector<TSNode> removed;
    for (const RowUse& use : uses)
    {
      const TSNode declaration = boundDeclaration(use.call);
      const Kind holder = kind(parent(declaration));
      if (ts_node_is_null(declaration) || bound[use.term].any ||
          lent[use.term] ||
          (holder != Kind::CompoundStatement && holder != Kind::InitStatement))
        continue;
      std::string name(f.declaredName(field(declaration, "declarator")));
      const TSNode scope = scopeOfStatement(declaration);
      if (name.empty() || mentions(loop, name, scope) ||
          mentionsBetween(scope, name, start(scope), start(declaration)))
        continue;
      names[use.term] = std::move(name);
      lent[use.term] = true;
      removed.push_back(declaration);
    }
    if (!nameRow(loop, entity, bound, bind, lent, names))
      return;
    // The entity stays only if the body reads it besides the lookups that go.
    std::vector<Binding> header;
    for (std::size_t i = 0; i < bound.size(); ++i)
      if (bind[i])
        header.push_back({names[i], bound[i].spelling, bound[i].any});
    std::vector<TSNode> going = removed;
    for (const RowUse& use : uses)
      going.push_back(use.call);
    if (readsName(field(loop, "body"), entity, going) ||
        !rowHeader(loop, row, {}, header, fix))
      rowHeader(loop, row, entity, header, fix);

    // The terms the list gains: all it binds for the system's own list, the
    // implied ones used for a typed one. An Any group is written once.
    std::string list;
    std::vector<std::string_view> groups;
    for (std::size_t i = 0; i < bound.size(); ++i)
    {
      if (!bind[i] || (!row.untyped && !bound[i].implied))
        continue;
      std::string_view term = bound[i].spelling;
      if (bound[i].any)
      {
        if (std::find(groups.begin(), groups.end(), bound[i].group) !=
            groups.end())
          continue;
        groups.push_back(bound[i].group);
        term = bound[i].group;
      }
      list += (list.empty() ? "" : ", ") + tidy(term);
    }
    if (row.untyped)
    {
      TSNode name = field(row.range, "function");
      if (kind(name) == Kind::FieldExpression)
        name = field(name, "field");
      fix.push_back(replace(name, "getEntities<" + list + ">"));
    }
    else if (!list.empty())
    {
      const uint32_t close = end(row.terms) - 1;
      fix.push_back(fileLines().edit(close, close, ", " + list));
    }
    for (const TSNode statement : removed)
    {
      const TSNode init = parent(statement);
      if (kind(init) == Kind::InitStatement)
        fix.push_back(
            fileLines().edit(start(init), start(nextNamed(init)), {}));
      else
        fix.push_back(removeStatement(statement));
    }
    for (const RowUse& use : uses)
    {
      bool gone = false;
      for (const TSNode statement : removed)
        gone = gone || within(use.call, statement);
      if (!gone)
        readThrough(use.call, names[use.term], bound[use.term].any, fix);
    }
  }

  // -------------------------------------------------------- filter-in-loop

  // Whether a row names a component, bound, as an Any member or excluded.
  // Spellings match whole or by their last names, so `sfs::Hitch` and
  // `Hitch` are taken for one type.
  bool rowNames(const Row& row, std::string_view type) const
  {
    const auto alike = [&](std::string_view term)
    { return sameType(term, type); };
    return std::any_of(row.bound.begin(),
                       row.bound.end(),
                       [&](const Bound& b) { return alike(b.spelling); }) ||
           std::any_of(row.excluded.begin(), row.excluded.end(), alike);
  }

  // Whether `node`, through parentheses, is a getComponent<T>() or
  // hasComponent<T>() of the loop's own entity; `out` gets it.
  bool loopLookup(TSNode node,
                  TSNode loop,
                  std::string_view entity,
                  Lookup& out) const
  {
    node = unparenthesised(node);
    if (kind(node) != Kind::CallExpression || !lookupOf(node, out))
      return false;
    const std::string_view name = text(out.name);
    return (name == "getComponent" || name == "hasComponent") &&
           kind(out.entity) == Kind::Identifier && text(out.entity) == entity &&
           same(declarationOf(out.entity, entity), loop);
  }

  // What a condition operand tests to be non-null: `x` of `x`,
  // `x != nullptr` or `nullptr != x`, through parentheses.
  TSNode nonNull(TSNode node) const
  {
    node = unparenthesised(node);
    if (!isBinary(node, "!="))
      return node;
    const TSNode left = unparenthesised(field(node, "left"));
    const TSNode right = unparenthesised(field(node, "right"));
    if (text(right) == "nullptr")
      return left;
    return text(left) == "nullptr" ? right : TSNode{};
  }

  // Whether `scope` adds or removes the component `type` on any entity.
  bool changesMembership(TSNode scope, std::string_view type) const
  {
    const std::string_view plain = lastName(trimmed(type));
    bool changes = false;
    walkTree(
        scope,
        [&](TSNode node)
        {
          if (kind(node) != Kind::CallExpression)
            return Next::Descend;
          TSNode templateArguments;
          const std::string_view name = f.calleeName(node, templateArguments);
          if (name != "addComponent" && name != "addTag" &&
              name != "removeComponent")
            return Next::Descend;
          const uint32_t count = ts_node_is_null(templateArguments)
                                     ? 0
                                     : namedCount(templateArguments);
          for (uint32_t i = 0; i < count && !changes; ++i)
            changes =
                lastName(trimmed(text(namedChild(templateArguments, i)))) ==
                plain;
          return changes ? Next::Stop : Next::Descend;
        });
    return changes;
  }

  // For a declaration of one variable set from a lookup of the loop's own
  // entity (`const T* t = e.getComponent<T>();`): the lookup, and the name.
  bool declaresLookup(TSNode declaration,
                      TSNode loop,
                      std::string_view entity,
                      Lookup& out,
                      std::string_view& name) const
  {
    const TSNode only = field(declaration, "declarator");
    if (kind(declaration) != Kind::Declaration ||
        declaratorCount(declaration) != 1 || kind(only) != Kind::InitDeclarator)
      return false;
    name = f.declaredName(only);
    return !name.empty() && loopLookup(field(only, "value"), loop, entity, out);
  }

  // Whether a condition operand tests that a component is missing: `!t` or
  // `t == nullptr` for a variable `names[i]` set from `declared[i]`, or `!`
  // or `== nullptr` of a lookup of the loop's own entity. `out` gets it.
  bool missingTest(TSNode node,
                   TSNode loop,
                   std::string_view entity,
                   Lookup& out) const
  {
    node = unparenthesised(node);
    TSNode tested{};
    if (isNot(node))
      tested = unparenthesised(field(node, "argument"));
    else if (isBinary(node, "=="))
    {
      const TSNode left = unparenthesised(field(node, "left"));
      const TSNode right = unparenthesised(field(node, "right"));
      tested = text(right) == "nullptr"  ? left
               : text(left) == "nullptr" ? right
                                         : TSNode{};
    }
    if (kind(tested) == Kind::Identifier)
    {
      for (std::size_t i = 0; i < m_declaredNames.size(); ++i)
        if (m_declaredNames[i] == text(tested))
        {
          out = m_declared[i];
          return true;
        }
      return false;
    }
    return !ts_node_is_null(tested) && loopLookup(tested, loop, entity, out);
  }

  // Whether `node` is an if statement without an else, not constexpr or
  // consteval.
  bool plainIf(TSNode node) const
  {
    return kind(node) == Kind::IfStatement &&
           ts_node_is_null(field(node, "alternative")) &&
           !spelledAs(text(node), {"if", "constexpr"}, false) &&
           !spelledAs(text(node), {"if", "consteval"}, false);
  }

  /*
   * filter-in-loop: a loop over `row` that skips the entities without a
   * component the row doesn't name. Its body is either one `if`, with no
   * else, that runs only when the loop's own entity has it (`if (T* t =
   * e.getComponent<T>())`, `if (T* t = ...; t && REST)`, or lookups and
   * hasComponent tests as `&&` operands of the condition); or a block that
   * opens with lookups of the entity (`const T* t = e.getComponent<T>();`)
   * and then `if (...) continue;`, with `!t`, `t == nullptr` or a negated
   * lookup or hasComponent among the `||` operands. The operands tested
   * before the last of them must be free of side effects, since they run
   * for the entities a list naming it would leave out. Not when the body
   * adds or removes one of them, which such a list mustn't see while it's
   * gone through; nor, for a lookup, where a component being removed is
   * still found (inRemovalHook()).
   */
  void filterInLoop(TSNode loop, const Row& row)
  {
    const TSNode declarator = field(loop, "declarator");
    const bool unpacked = kind(declarator) == Kind::StructuredBindingDeclarator;
    const std::string_view entity =
        unpacked ? text(namedChild(declarator, 0)) : f.declaredName(declarator);
    const TSNode body = field(loop, "body");
    if (entity.empty() || ts_node_is_null(body) || row.unknownTerms)
      return;
    statementsOf(body, m_statements);

    // The lookups of components the row doesn't name that the loop needs to
    // go on, and for the one-if form the if, the pointer its condition
    // declares, and whether the condition is that test alone.
    m_needed.clear();
    const bool removing = inRemovalHook(loop);
    const auto need = [&](const Lookup& lookup)
    {
      const std::string_view type = text(lookup.type);
      if (type.ends_with("...") || rowNames(row, type) ||
          (removing && text(lookup.name) == "getComponent"))
        return false;
      m_needed.push_back(lookup);
      return true;
    };
    // The operands before the last needed one run for every entity.
    const auto pureBefore = [&](std::size_t last)
    {
      for (std::size_t i = 0; i < last; ++i)
        if (!sideEffectFree(m_terms[i]))
          return false;
      return true;
    };
    TSNode statement{};
    TSNode declaration{};
    bool whole = false;
    m_terms.clear();
    if (m_statements.size() == 1 && plainIf(m_statements[0]))
    {
      statement = m_statements[0];
      const TSNode condition = field(statement, "condition");
      const TSNode init = field(condition, "initializer");
      const TSNode value = field(condition, "value");
      Lookup lookup;
      if (kind(value) == Kind::Declaration)
      {
        // `T* t = e.getComponent<T>()`
        if (!ts_node_is_null(init) ||
            !loopLookup(field(value, "value"), loop, entity, lookup))
          return;
        declaration = value;
        whole = true;
        need(lookup);
      }
      else if (!ts_node_is_null(init))
      {
        // `T* t = e.getComponent<T>(); t && ...`
        std::string_view name;
        if (namedCount(init) != 1 ||
            !declaresLookup(namedChild(init, 0), loop, entity, lookup, name))
          return;
        declaration = namedChild(init, 0);
        operands(value, "&&", m_terms);
        for (std::size_t i = 0; i < m_terms.size(); ++i)
        {
          const TSNode tested = nonNull(m_terms[i]);
          if (kind(tested) == Kind::Identifier && text(tested) == name)
          {
            if (pureBefore(i))
              need(lookup);
            break;
          }
        }
        whole = m_terms.size() == 1;
      }
      else
      {
        operands(value, "&&", m_terms);
        std::size_t last = 0;
        for (std::size_t i = 0; i < m_terms.size(); ++i)
          if (loopLookup(nonNull(m_terms[i]), loop, entity, lookup) &&
              need(lookup))
            last = i;
        if (!pureBefore(last))
          return;
        whole = m_terms.size() == 1;
      }
    }
    else if (kind(body) == Kind::CompoundStatement)
    {
      // Lookups, then `if (...) continue;`.
      m_declared.clear();
      m_declaredNames.clear();
      std::size_t at = 0;
      Lookup lookup;
      std::string_view name;
      for (; at < m_statements.size() &&
             declaresLookup(m_statements[at], loop, entity, lookup, name);
           ++at)
      {
        m_declared.push_back(lookup);
        m_declaredNames.push_back(name);
      }
      if (at == m_statements.size() || !plainIf(m_statements[at]))
        return;
      const TSNode test = m_statements[at];
      const TSNode condition = field(test, "condition");
      const TSNode value = field(condition, "value");
      TSNode jump = field(test, "consequence");
      if (kind(jump) == Kind::CompoundStatement)
        jump = soleStatement(jump);
      if (!ts_node_is_null(field(condition, "initializer")) ||
          kind(value) == Kind::Declaration ||
          kind(jump) != Kind::ContinueStatement)
        return;
      operands(value, "||", m_terms);
      std::size_t last = 0;
      for (std::size_t i = 0; i < m_terms.size(); ++i)
        if (missingTest(m_terms[i], loop, entity, lookup) && need(lookup))
          last = i;
      if (!pureBefore(last))
        return;
    }
    if (m_needed.empty())
      return;

    // Each component once, in the order the loop tests them.
    std::vector<std::string> shown;
    for (const Lookup& lookup : m_needed)
    {
      const std::string_view type = text(lookup.type);
      if (changesMembership(body, type))
        return;
      if (std::none_of(shown.begin(),
                       shown.end(),
                       [&](const std::string& known)
                       { return sameType(known, type); }))
        shown.push_back(tidy(unconst(type)));
    }
    std::string without;
    std::string listed;
    for (std::size_t i = 0; i < shown.size(); ++i)
    {
      if (i > 0)
      {
        without += i + 1 == shown.size() ? " or " : ", ";
        listed += i + 1 == shown.size() ? " and " : ", ";
      }
      const char first = lastName(shown[i]).front();
      without += std::string_view("AEIOU").find(first) != std::string_view::npos
                     ? "an "
                     : "a ";
      without += shown[i];
      listed += shown[i];
    }
    const std::string have = shown.size() == 1 ? "one." : "them.";

    // A query or a hierarchy walk written in the loop takes the components in
    // its terms; a shared list (a helper's or a variable's) or a system's own
    // isn't changed for one loop.
    TSNode templateArguments;
    const bool query =
        row.walk || (!row.untyped && !row.everything &&
                     f.calleeName(row.range, templateArguments) == "query");
    const std::string into = row.walk ? "the walk's terms" : "the query";
    std::string message;
    if (row.everything)
      message = "This loop goes through every entity and skips those without " +
                without + ". Go through a query that lists " + listed +
                " instead; a query leaves out entities created this frame "
                "and not yet added, so check the loop doesn't need those.";
    else
    {
      message = "This loop goes through every entity " +
                std::string(row.walk ? "in the walk"
                            : query  ? "in the query"
                                     : "of the system") +
                " and skips those without " + without + ". ";
      if (query && row.inPlace)
        message += "Add " + listed + " to " + into +
                   " so it only visits the entities that have " + have;
      else
        message += "Go through a query that also lists " + listed +
                   " so it only visits the entities that have " + have;
    }

    std::vector<TextEdit> fix;
    if (query && row.inPlace && whole && m_needed.size() == 1 &&
        !ts_node_is_null(statement))
      fixFilter(loop,
                row,
                statement,
                declaration,
                text(m_needed[0].type),
                entity,
                unpacked,
                fix);
    // On the loop's `for`, where an sfs-ignore above or beside the loop finds
    // it.
    const TSNode at = nthChild(loop, 0);
    if (fix.empty())
      f.report(RuleId::FilterInLoop, at, std::move(message));
    else
      f.report(RuleId::FilterInLoop,
               at,
               std::move(message),
               "Add " + shown[0] + " to " + into,
               std::move(fix));
  }

  // Whether evaluating `node` changes nothing: no assignment, `++` or `--`,
  // `new` or `delete`, and no call but an entity's lookups and tests
  // (getComponent, hasComponent, isValid, matches).
  bool sideEffectFree(TSNode node) const
  {
    bool pure = true;
    walkTree(node,
             [&](TSNode inner)
             {
               const Kind k = kind(inner);
               const std::string_view type = typeName(inner);
               TSNode templateArguments;
               const std::string_view callee =
                   k == Kind::CallExpression
                       ? f.calleeName(inner, templateArguments)
                       : std::string_view{};
               pure = k != Kind::AssignmentExpression &&
                      k != Kind::UpdateExpression && type != "new_expression" &&
                      type != "delete_expression" &&
                      (k != Kind::CallExpression || callee == "getComponent" ||
                       callee == "hasComponent" || callee == "isValid" ||
                       callee == "matches");
               return pure ? Next::Descend : Next::Stop;
             });
    return pure;
  }

  /*
   * The fix of filter-in-loop for a query or hierarchy walk written in the
   * loop: T joins its terms, the if goes, and its body becomes the loop's. A
   * pointer the condition declared becomes the row's binding of T (`t->` to
   * `t.`, `*t` to `t`, other uses `&t`), so the loop unpacks its row. No fix
   * when a use can't be rewritten exactly (assigned, address taken, in a
   * lambda, redeclared, a const pointer passed on), a comment would go, or a
   * binding name is taken.
   */
  void fixFilter(TSNode loop,
                 const Row& row,
                 TSNode statement,
                 TSNode declaration,
                 std::string_view type,
                 std::string_view entity,
                 bool unpacked,
                 std::vector<TextEdit>& fix)
  {
    const TSNode body = field(loop, "body");
    const TSNode consequence = field(statement, "consequence");
    bool comment = false;
    walkTree(body,
             [&](TSNode node)
             {
               if (same(node, consequence))
                 return Next::Skip;
               comment = kind(node) == Kind::Comment;
               return comment ? Next::Stop : Next::Descend;
             });
    // Re-indenting would change a raw string's text or a continued line.
    bool verbatim = false;
    walkTree(consequence,
             [&](TSNode node)
             {
               verbatim =
                   std::string_view(typeName(node)) == "raw_string_literal";
               return verbatim ? Next::Stop : Next::Descend;
             });
    const std::string_view moving = text(consequence);
    if (comment || verbatim || ts_node_is_null(consequence) ||
        moving.find("\\\n") != std::string_view::npos ||
        moving.find("\\\r\n") != std::string_view::npos)
      return;

    // The pointer's uses, rewritten to read the binding: byte ranges in the
    // consequence, in order.
    struct Use
    {
      uint32_t from, to;
      std::string text;
    };
    std::vector<Use> uses;
    std::string name;
    if (!ts_node_is_null(declaration))
    {
      TSNode pointer = field(declaration, "declarator");
      if (kind(pointer) == Kind::InitDeclarator)
        pointer = field(pointer, "declarator");
      if (kind(pointer) != Kind::PointerDeclarator)
        return;
      name = f.declaredName(pointer);
      const bool constant = constType(declaration);
      bool ok = !name.empty();
      walkTree(consequence,
               [&](TSNode node)
               {
                 if (kind(node) != Kind::Identifier || text(node) != name)
                   return Next::Descend;
                 const TSNode up = parent(node);
                 if (declaresHere(node) || inLambdaWithin(node, consequence) ||
                     kind(up) == Kind::QualifiedIdentifier ||
                     kind(up) == Kind::UpdateExpression ||
                     (kind(up) == Kind::AssignmentExpression &&
                      same(field(up, "left"), node)))
                   ok = false;
                 else if (kind(up) == Kind::FieldExpression &&
                          same(field(up, "argument"), node))
                 {
                   const TSNode op = operatorOf(up);
                   ok = text(op) == "->";
                   uses.push_back({start(op), end(op), "."});
                 }
                 else if (kind(up) == Kind::PointerExpression &&
                          same(field(up, "argument"), node))
                 {
                   ok = text(field(up, "operator")) == "*";
                   uses.push_back({start(up), end(up), name});
                 }
                 else if (constant)
                   ok = false;
                 else
                   uses.push_back(
                       {start(node),
                        end(node),
                        postfixObject(node) ? "(&" + name + ")" : "&" + name});
                 return ok ? Next::Descend : Next::Stop;
               });
      if (!ok)
        return;
    }

    // The header binds T when there's a pointer to bind it to, or when the
    // loop unpacks its row already. The entity stays only if what's left of
    // the body reads it.
    std::vector<Binding> header;
    if (unpacked)
    {
      const TSNode declarator = field(loop, "declarator");
      const uint32_t count = namedCount(declarator);
      if (count != row.bound.size() + 1)
        return;
      if (name.empty())
      {
        name = nameFor(type);
        if (name.empty() || mentions(loop, name))
          return;
      }
      for (uint32_t i = 0; i < count; ++i)
        if (text(namedChild(declarator, i)) == name)
          return;
      for (uint32_t i = 1; i < count; ++i)
        header.push_back({std::string(text(namedChild(declarator, i))),
                          row.bound[i - 1].spelling,
                          row.bound[i - 1].any});
    }
    else if (!name.empty())
    {
      if (!unpackableEntity(loop, entity))
        return;
      std::vector<Bound> bound = row.bound;
      bound.push_back({type, false, {}});
      const std::vector<bool> bind(bound.size(), true);
      std::vector<bool> lent(bound.size(), false);
      std::vector<std::string> names(bound.size());
      lent.back() = true;
      names.back() = name;
      if (!nameRow(loop, entity, bound, bind, lent, names))
        return;
      for (std::size_t i = 0; i + 1 < bound.size(); ++i)
        header.push_back({names[i], bound[i].spelling, bound[i].any});
    }
    if (!name.empty())
    {
      header.push_back({name, type, false});
      if (readsName(consequence, entity) ||
          !rowHeader(loop, row, {}, header, fix))
        rowHeader(loop, row, entity, header, fix);
    }

    // T joins the query's terms.
    const uint32_t close = end(row.terms) - 1;
    fix.push_back(fileLines().edit(
        close, close, (namedCount(row.terms) > 0 ? ", " : "") + tidy(type)));

    // The consequence takes the if's place: a block as the loop's body, at
    // the loop's indentation; a statement where the if was.
    const Lines& lines = fileLines();
    const bool block = kind(consequence) == Kind::CompoundStatement;
    const TSNode replaced = block ? body : statement;
    const std::string_view indent =
        lines.indentOf(start(block ? loop : statement));
    const uint32_t from = start(consequence);
    std::string moved =
        lines.startsLine(from)
            ? std::string(f.source.substr(
                  lines.lineStart(from), from - lines.lineStart(from)))
            : std::string(lines.indentOf(from));
    uint32_t at = from;
    for (const Use& use : uses)
    {
      moved += f.source.substr(at, use.from - at);
      moved += use.text;
      at = use.to;
    }
    moved += f.source.substr(at, end(consequence) - at);
    std::string replacement = reindent(moved, indent);
    while (!replacement.empty() && replacement.back() == '\n')
      replacement.pop_back();
    uint32_t replaceFrom = start(replaced);
    if (lines.startsLine(replaceFrom))
      replaceFrom = lines.lineStart(replaceFrom);
    else if (replacement.starts_with(indent))
      replacement.erase(0, indent.size());
    fix.push_back(lines.edit(replaceFrom, end(replaced), replacement));
  }

  // ----------------------------------------------------- unused-row-entity

  // unused-row-entity on a loop over `row` (rowOf()) that unpacks its rows
  // as `[entity, components...]` and never reads the entity: the header
  // binds the components alone (rowHeader()). Not for a system's own list,
  // whose rows bind nothing, or a binding that isn't the row's shape.
  void unusedRowEntity(TSNode loop, const Row& row)
  {
    const TSNode unpacking = unpackingOf(loop);
    const TSNode body = field(loop, "body");
    if (ts_node_is_null(unpacking) || ts_node_is_null(body) || row.everything ||
        row.untyped || row.unknownTerms)
      return;
    std::vector<Binding> header;
    for (const Bound& bound : row.bound)
      if (!bound.implied)
        header.push_back(
            {std::string(text(namedChild(unpacking, header.size() + 1))),
             bound.spelling,
             bound.any});
    const TSNode entity = namedChild(unpacking, 0);
    if (header.empty() || namedCount(unpacking) != header.size() + 1 ||
        kind(entity) != Kind::Identifier || readsName(body, text(entity)))
      return;
    std::vector<TextEdit> fix;
    std::string message =
        "This loop unpacks each row's entity but never uses it. ";
    if (header.size() == 1 && !header[0].any)
      message += "Loop over the " + tidy(unconst(header[0].spelling)) +
                 " alone: `" + tidy(header[0].spelling) + "& " +
                 header[0].name + "`.";
    else
      message += "Loop over the components alone, with `.components()`.";
    if (rowHeader(loop, row, {}, header, fix))
      f.report(RuleId::UnusedRowEntity,
               entity,
               std::move(message),
               "Loop over the components alone",
               std::move(fix));
    else
      f.report(RuleId::UnusedRowEntity, entity, std::move(message));
  }

  // ---------------------------------------------------- row-component-copy

  // Whether every use of `name` in `scope` only reads it: through members
  // (`name.a.b`), parentheses or an index, as an operand, a condition, a
  // returned value or a copy's initialiser. Anything else (an assignment, a
  // call or argument, `&`, a reference bound to it) may change it.
  bool onlyRead(TSNode scope, std::string_view name) const
  {
    bool read = true;
    walkTree(scope,
             [&](TSNode node)
             {
               if (kind(node) != Kind::Identifier || text(node) != name)
                 return Next::Descend;
               TSNode top = node;
               for (TSNode up = parent(top);; up = parent(top))
               {
                 const Kind k = kind(up);
                 if (!((k == Kind::FieldExpression &&
                        same(field(up, "argument"), top) &&
                        text(operatorOf(up)) == ".") ||
                       k == Kind::ParenthesizedExpression ||
                       (k == Kind::SubscriptExpression &&
                        same(field(up, "argument"), top))))
                   break;
                 top = up;
               }
               const TSNode up = parent(top);
               switch (kind(up))
               {
               case Kind::BinaryExpression:
                 // `stream >> t.x` writes its right operand.
                 read = text(field(up, "operator")) != ">>" ||
                        same(field(up, "left"), top);
                 break;
               case Kind::ConditionalExpression:
                 // A branch may be assigned to: `(c ? t : u).x = 1`.
                 read = same(field(up, "condition"), top);
                 break;
               case Kind::UnaryExpression:
               case Kind::ConditionClause:
               case Kind::ReturnStatement:
               case Kind::SubscriptExpression:
                 break;
               case Kind::AssignmentExpression:
                 read = same(field(up, "right"), top);
                 break;
               case Kind::InitDeclarator:
                 read = same(field(up, "value"), top) &&
                        kind(field(up, "declarator")) == Kind::Identifier;
                 break;
               default:
                 read = false;
               }
               return read ? Next::Descend : Next::Stop;
             });
    return read;
  }

  // row-component-copy on a loop over a query's view or a typed walk (rowOf())
  // of one required component whose loop variable copies it (`T t`, the row
  // converting to T). A copy the body only reads is read in place: the fix
  // binds `const T&`. A written one may be scratch on purpose, so it's
  // reported without a fix, and not at all for a const term, whose rows
  // can't be written.
  void rowComponentCopy(TSNode loop, const Row& row)
  {
    const TSNode declarator = field(loop, "declarator");
    const TSNode type = typeOf(loop);
    const TSNode body = field(loop, "body");
    if (kind(declarator) != Kind::Identifier || ts_node_is_null(body) ||
        row.everything || row.untyped || row.unknownTerms ||
        std::strcmp(typeName(type), "placeholder_type_specifier") == 0 ||
        !takesComponents(loop, row))
      return;
    const Bound* sole = nullptr;
    for (const Bound& bound : row.bound)
      if (!bound.implied)
      {
        if (sole)
          return;
        sole = &bound;
      }
    if (!sole || sole->any || !sameType(text(type), sole->spelling))
      return;
    const std::string spelled = tidy(unconst(text(type)));
    const std::string name(text(declarator));
    if (constType(loop) || onlyRead(body, name))
    {
      std::vector<TextEdit> fix{
          fileLines().edit(headerStart(loop),
                           end(declarator),
                           "const " + spelled + "& " + name)};
      f.report(RuleId::RowComponentCopy,
               declarator,
               "This loop copies each " + spelled +
                   " but only reads it. Read it in place with const " +
                   spelled + "&.",
               "Read it by const reference",
               std::move(fix));
    }
    else if (unconst(sole->spelling) == trimmed(sole->spelling))
      f.report(RuleId::RowComponentCopy,
               declarator,
               "This loop copies each " + spelled +
                   ", so changes to the copy don't reach the component. If "
                   "the copy is scratch, keep it; otherwise use " +
                   spelled + "&.");
  }

  // -------------------------------------------------- setup-in-constructor

  // A file the set-up fix edits: its text and lines, and its path (empty for
  // the linted file).
  struct Doc
  {
    std::string_view source;
    const Lines* lines;
    std::filesystem::path file;

    std::string_view text(TSNode node) const
    {
      return codegen::nodeText(source, node);
    }
    TextEdit edit(uint32_t from, uint32_t to, std::string replacement) const
    {
      return lines->edit(from, to, std::move(replacement), file);
    }
  };

  // A file the rules read besides the linted one (a header): its text,
  // lines and tree.
  struct ReadFile
  {
    std::filesystem::path path;
    std::string text;
    Lines lines;
    std::unique_ptr<TSTree, TreeDeleter> tree;
  };
  std::vector<std::unique_ptr<ReadFile>> readFiles; // read this run

  // `path` read (through LintFile::read) and parsed once a run, its query
  // aliases added to the file's; null when it can't be read.
  const ReadFile* readFile(const std::filesystem::path& path)
  {
    const std::filesystem::path normal = path.lexically_normal();
    for (const std::unique_ptr<ReadFile>& known : readFiles)
      if (known->path == normal)
        return known->tree ? known.get() : nullptr;
    // A parser for the files it reads, made once.
    static const ParserPtr parser = makeCppParser();
    ReadFile& file = *readFiles.emplace_back(std::make_unique<ReadFile>());
    file.path = normal;
    file.text = (*f.read)(normal);
    if (file.text.empty())
      return nullptr;
    file.tree.reset(
        ts_parser_parse_string(parser.get(),
                               nullptr,
                               file.text.data(),
                               static_cast<uint32_t>(file.text.size())));
    if (!file.tree)
      return nullptr;
    file.lines.set(file.text);
    findQueryAliases(file.tree.get(), file.text, file.path, f.aliases);
    return &file;
  }

  std::optional<std::vector<std::filesystem::path>> includedFiles;

  // The files the linted file includes with "...", each found beside it or
  // under an include folder of a folder above it, in order.
  const std::vector<std::filesystem::path>& includes()
  {
    if (includedFiles)
      return *includedFiles;
    includedFiles.emplace();
    const std::filesystem::path folder = f.path->parent_path();
    std::error_code error;
    for (std::size_t at = f.source.find("#include"); at != std::string::npos;
         at = f.source.find("#include", at + 8))
    {
      const std::size_t open = f.source.find('"', at);
      const std::size_t line = f.source.find('\n', at);
      if (open == std::string::npos || open > line)
        continue;
      const std::size_t close = f.source.find('"', open + 1);
      if (close == std::string::npos || close > line)
        continue;
      const std::filesystem::path included(
          std::string(f.source.substr(open + 1, close - open - 1)));
      std::vector<std::filesystem::path> candidates = {folder / included};
      std::filesystem::path above = folder;
      for (int depth = 0; depth < 10 && !above.empty(); ++depth)
      {
        candidates.push_back(above / included);
        candidates.push_back(above / "include" / included);
        if (above == above.parent_path())
          break;
        above = above.parent_path();
      }
      for (const std::filesystem::path& candidate : candidates)
        if (std::filesystem::is_regular_file(candidate, error))
        {
          includedFiles->push_back(candidate.lexically_normal());
          break;
        }
    }
    return *includedFiles;
  }

  // A class as found in its header.
  struct ClassHeader
  {
    std::string owner;
    const ReadFile* file = nullptr;
    TSNode node{};
  };
  std::vector<std::unique_ptr<ClassHeader>> classHeaders; // found this run

  // The header declaring class `owner`: the project index's, else one the
  // file includes that declares it. Null when there's none.
  const ClassHeader* classHeader(std::string_view owner)
  {
    for (const std::unique_ptr<ClassHeader>& known : classHeaders)
      if (known->owner == owner)
        return ts_node_is_null(known->node) ? nullptr : known.get();
    ClassHeader& header =
        *classHeaders.emplace_back(std::make_unique<ClassHeader>());
    header.owner = std::string(owner);
    const auto find = [&](const std::filesystem::path& path)
    {
      header.file = readFile(path);
      header.node = header.file
                        ? classIn(ts_tree_root_node(header.file->tree.get()),
                                  header.file->text,
                                  owner)
                        : TSNode{};
      return !ts_node_is_null(header.node);
    };
    if (const std::filesystem::path known = f.index->headerOf(owner);
        !known.empty())
      find(known);
    else
    {
      const std::string classMark = "class " + std::string(owner);
      const std::string structMark = "struct " + std::string(owner);
      for (const std::filesystem::path& path : includes())
      {
        const ReadFile* file = readFile(path);
        if (file &&
            (file->text.find(classMark) != std::string::npos ||
             file->text.find(structMark) != std::string::npos) &&
            find(path))
          break;
      }
    }
    return ts_node_is_null(header.node) ? nullptr : &header;
  }

  // The class called `owner` with a body in a tree, or null.
  TSNode classIn(TSNode root, std::string_view source, std::string_view owner)
  {
    TSNode found{};
    walkTree(root,
             [&](TSNode node)
             {
               const Kind k = kind(node);
               if (k == Kind::ClassSpecifier &&
                   codegen::nodeText(source, field(node, "name")) == owner &&
                   !ts_node_is_null(field(node, "body")))
               {
                 found = node;
                 return Next::Stop;
               }
               // Classes sit in namespaces and templates, not in functions.
               const bool holds =
                   k == Kind::TranslationUnit ||
                   k == Kind::NamespaceDefinition ||
                   k == Kind::TemplateDeclaration || k == Kind::Declaration ||
                   std::strcmp(typeName(node), "declaration_list") == 0;
               return holds ? Next::Descend : Next::Skip;
             });
    return found;
  }

  // Whether a class's base clause names SFS's System.
  bool derivesFromSystem(TSNode classNode, const Doc& doc) const
  {
    const bool usingSfs =
        doc.source.find("using namespace sfs;") != std::string_view::npos;
    bool inSfs = false;
    for (TSNode up = parent(classNode); !ts_node_is_null(up); up = parent(up))
      inSfs = inSfs || (kind(up) == Kind::NamespaceDefinition &&
                        codegen::isSfsNamespace(doc.text(field(up, "name"))));
    const uint32_t count = namedCount(classNode);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode clause = namedChild(classNode, i);
      if (kind(clause) != Kind::BaseClassClause)
        continue;
      const uint32_t bases = namedCount(clause);
      for (uint32_t b = 0; b < bases; ++b)
      {
        const std::string_view base = doc.text(namedChild(clause, b));
        if (spelledAs(base, {"sfs::System"}) ||
            (spelledAs(base, {"System"}) && (usingSfs || inSfs)))
          return true;
      }
    }
    return false;
  }

  // A member function's declarator name in a class body member, or null.
  TSNode memberName(TSNode member) const
  {
    return field(functionDeclarator(member), "declarator");
  }

  // The function declarator of a function's definition or declaration,
  // through the pointer and reference declarators of its return type; null
  // when there's none.
  TSNode functionDeclarator(TSNode function) const
  {
    TSNode declarator = field(function, "declarator");
    while (!ts_node_is_null(declarator) &&
           kind(declarator) != Kind::FunctionDeclarator)
    {
      const uint32_t count = namedCount(declarator);
      declarator = kind(declarator) == Kind::ReferenceDeclarator && count > 0
                       ? namedChild(declarator, count - 1)
                       : field(declarator, "declarator");
    }
    return declarator;
  }

  // The comment lines directly above `node` in its parent, each on its own
  // lines: where removing `node` with them starts.
  uint32_t commentsAbove(TSNode node, const Lines& lines) const
  {
    uint32_t from = start(node);
    for (TSNode before = prevNamed(node); kind(before) == Kind::Comment;
         before = prevNamed(before))
    {
      if (!lines.startsLine(start(before)) ||
          lines.point(end(before)).row + 1 != lines.point(from).row)
        break;
      from = start(before);
    }
    return from;
  }

  // The edit removing a class member with the comment lines above it, as
  // whole lines; when that leaves its access section empty, the section's
  // label goes too.
  TextEdit removeMember(TSNode member, const Doc& doc) const
  {
    const Lines& lines = *doc.lines;
    TSNode first = member;
    for (TSNode before = prevNamed(member);
         kind(before) == Kind::Comment && lines.startsLine(start(before)) &&
         lines.point(end(before)).row + 1 == lines.point(start(first)).row;
         before = prevNamed(before))
      first = before;
    uint32_t from = lines.lineStart(start(first));
    uint32_t to = lines.nextLine(end(member));
    const TSNode label = prevNamed(first);
    const TSNode next = nextNamed(member);
    if (kind(label) == Kind::AccessSpecifier &&
        lines.startsLine(start(label)) &&
        (ts_node_is_null(next) || kind(next) == Kind::AccessSpecifier))
    {
      from = lines.lineStart(start(label));
      if (!ts_node_is_null(next) && lines.startsLine(start(next)))
        to = lines.lineStart(start(next));
    }
    else if (from > 0 && to < doc.source.size() && lines.endsLine(to))
    {
      // A blank line after it goes too when one is before it, or it follows
      // a label or the opening brace: no blank lines meet or start a section.
      const std::string_view before = trimmed(doc.source.substr(
          lines.lineStart(from - 1), from - lines.lineStart(from - 1)));
      if (before.empty() || before.ends_with(':') || before.ends_with('{'))
        to = lines.nextLine(to);
    }
    return doc.edit(from, to, {});
  }

  // Where `member` goes in a class's protected section, as an edit: after
  // `protected:`, else as a new section before the one after `public:`, else
  // at the end. False when the class body's shape isn't one it can write in.
  bool addProtected(TSNode classNode,
                    const Doc& doc,
                    const std::string& member,
                    std::vector<TextEdit>& fix) const
  {
    const Lines& lines = *doc.lines;
    const TSNode body = field(classNode, "body");
    const uint32_t count = namedCount(body);
    TSNode publicSpec{};
    TSNode next{};
    std::string_view specIndent;
    bool haveSpec = false;
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = namedChild(body, i);
      if (kind(child) != Kind::AccessSpecifier)
        continue;
      if (!haveSpec)
        specIndent = lines.indentOf(start(child));
      haveSpec = true;
      const std::string_view access = trimmed(doc.text(child));
      if (access == "protected")
      {
        fix.push_back(doc.edit(lines.nextLine(start(child)),
                               lines.nextLine(start(child)),
                               member));
        return true;
      }
      if (!ts_node_is_null(publicSpec) && ts_node_is_null(next))
        next = child;
      if (access == "public")
        publicSpec = child;
    }
    if (!haveSpec)
      specIndent = lines.indentOf(start(classNode));
    if (!ts_node_is_null(next) && lines.startsLine(start(next)))
    {
      const uint32_t at = lines.lineStart(start(next));
      fix.push_back(doc.edit(
          at, at, std::string(specIndent) + "protected:\n" + member + "\n"));
      return true;
    }
    // The closing brace, on a line of its own.
    const uint32_t close = end(body) - 1;
    if (doc.source[close] != '}' || !lines.startsLine(close))
      return false;
    const uint32_t at = lines.lineStart(close);
    fix.push_back(doc.edit(
        at, at, "\n" + std::string(specIndent) + "protected:\n" + member));
    return true;
  }

  // The indentation of a class's members.
  std::string memberIndent(TSNode classNode, const Doc& doc) const
  {
    const TSNode body = field(classNode, "body");
    const uint32_t count = namedCount(body);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = namedChild(body, i);
      if (kind(child) != Kind::AccessSpecifier &&
          kind(child) != Kind::Comment && doc.lines->startsLine(start(child)))
        return std::string(doc.lines->indentOf(start(child)));
    }
    return std::string(doc.lines->indentOf(start(classNode))) + "  ";
  }

  void setupInConstructor(TSNode ctor)
  {
    const TSNode declarator = field(ctor, "declarator");
    const TSNode nameNode = field(declarator, "declarator");
    const bool outOfLine = kind(nameNode) == Kind::QualifiedIdentifier;
    std::string_view scope; // "X" of X::X, "ns::X" of ns::X::X
    std::string_view owner;
    if (outOfLine)
    {
      const std::string_view written = text(nameNode);
      const std::size_t colons = written.rfind("::");
      scope = trimmed(written.substr(0, colons));
      owner = lastName(scope);
    }
    else
      owner = text(nameNode);

    TSNode body = field(ctor, "body");
    bool tryBlock = false;
    if (ts_node_is_null(body))
    {
      const uint32_t count = namedCount(ctor);
      for (uint32_t i = 0; i < count && ts_node_is_null(body); ++i)
        if (kind(namedChild(ctor, i)) == Kind::TryStatement)
        {
          body = field(namedChild(ctor, i), "body");
          tryBlock = true;
        }
    }
    if (ts_node_is_null(body))
      return;

    // The set-up calls, unqualified or through `this`.
    std::vector<TSNode> calls;
    bool direct = true;
    walkTree(
        body,
        [&](TSNode node)
        {
          if (kind(node) != Kind::CallExpression)
            return Next::Descend;
          TSNode templateArguments;
          const std::string_view callee = f.calleeName(node, templateArguments);
          const TSNode receiver = f.receiverOf(node);
          const bool templated = !ts_node_is_null(templateArguments);
          if ((ts_node_is_null(receiver) || kind(receiver) == Kind::This) &&
              (((callee == "registerComponent" || callee == "observe") &&
                templated) ||
               (callee == "setTimeStep" && !templated)))
          {
            calls.push_back(node);
            const TSNode statement = parent(node);
            direct = direct && kind(statement) == Kind::ExpressionStatement &&
                     same(parent(statement), body);
          }
          return Next::Descend;
        });
    if (calls.empty())
      return;

    // The class: around the constructor, in this file, or in its header.
    Doc here{f.source, &fileLines(), {}};
    Doc classDoc = here;
    TSNode classNode{};
    if (!outOfLine)
    {
      const TSNode members = parent(ctor);
      if (kind(members) == Kind::FieldDeclarationList)
        classNode = parent(members);
    }
    else if (const FileClass* inFile = f.fileClass(owner))
      classNode = inFile->node;
    else if (const ClassHeader* header = classHeader(owner))
    {
      classDoc = {header->file->text, &header->file->lines, header->file->path};
      classNode = header->node;
    }
    const bool system = (!ts_node_is_null(classNode) &&
                         derivesFromSystem(classNode, classDoc)) ||
                        f.isSystemClass(owner);
    if (!system)
      return;

    std::vector<TextEdit> fix;
    if (direct && !tryBlock && !ts_node_is_null(classNode))
      moveSetUp(ctor,
                body,
                outOfLine,
                scope,
                owner,
                calls,
                classNode,
                here,
                classDoc,
                fix);
    for (const TSNode call : calls)
    {
      const std::string message =
          "A system should not set itself up in its constructor. Move its "
          "registerComponent, observe and setTimeStep calls to create(), "
          "which runs once when the system is added.";
      if (fix.empty())
        f.report(RuleId::SetupInConstructor, call, message);
      else
        f.report(RuleId::SetupInConstructor,
                 call,
                 message,
                 "Move the set-up to create()",
                 fix);
    }
  }

  // The fix of setup-in-constructor: the calls, with the comments above and
  // beside them, move to the start of create() (made if there's none), and a
  // constructor left with nothing to do goes. Leaves `fix` empty when it
  // can't be written.
  void moveSetUp(TSNode ctor,
                 TSNode body,
                 bool outOfLine,
                 std::string_view scope,
                 std::string_view owner,
                 const std::vector<TSNode>& calls,
                 TSNode classNode,
                 const Doc& here,
                 const Doc& classDoc,
                 std::vector<TextEdit>& fix)
  {
    const Lines& lines = *here.lines;
    // Not for templates.
    if (kind(parent(classNode)) == Kind::TemplateDeclaration ||
        kind(parent(ctor)) == Kind::TemplateDeclaration)
      return;
    // A call naming a constructor parameter can't leave it.
    const TSNode parameters = parametersOf(ctor);
    const uint32_t parameterCount =
        ts_node_is_null(parameters) ? 0 : namedCount(parameters);
    for (uint32_t i = 0; i < parameterCount; ++i)
    {
      const std::string_view parameter =
          f.declaredName(field(namedChild(parameters, i), "declarator"));
      for (const TSNode call : calls)
        if (!parameter.empty() && mentions(call, parameter))
          return;
    }
    // Nor one naming a local the constructor declared before it.
    for (const TSNode call : calls)
    {
      bool local = false;
      walkTree(call,
               [&](TSNode node)
               {
                 if (kind(node) == Kind::Identifier)
                   for (uint32_t i = 0; !local && i < namedCount(body); ++i)
                   {
                     const TSNode earlier = namedChild(body, i);
                     if (start(earlier) >= start(call))
                       break;
                     local = declares(earlier, text(node));
                   }
                 return local ? Next::Stop : Next::Descend;
               });
      if (local)
        return;
    }

    // One user-declared constructor, and create() in the class or this file.
    const TSNode members = field(classNode, "body");
    const uint32_t memberCount = namedCount(members);
    int constructors = 0;
    TSNode declaration{};
    TSNode createBody{};
    bool createDeclared = false;
    for (uint32_t i = 0; i < memberCount; ++i)
    {
      const TSNode member = namedChild(members, i);
      const Kind k = kind(member);
      if (k != Kind::Declaration && k != Kind::FieldDeclaration &&
          k != Kind::FunctionDefinition)
        continue;
      const TSNode name = memberName(member);
      const std::string_view spelled = classDoc.text(name);
      if (kind(name) == Kind::Identifier && spelled == owner)
      {
        bool deleted = false;
        const uint32_t parts = namedCount(member);
        for (uint32_t p = 0; p < parts; ++p)
          deleted = deleted ||
                    kind(namedChild(member, p)) == Kind::DeleteMethodClause;
        if (!deleted)
          ++constructors;
        if (k == Kind::Declaration &&
            namedCount(field(field(member, "declarator"), "parameters")) == 0)
          declaration = member;
      }
      else if (spelled == "create")
      {
        createDeclared = true;
        if (k == Kind::FunctionDefinition)
          createBody = field(member, "body");
      }
    }
    if (constructors != 1)
      return;
    Doc createDoc = classDoc;
    if (createDeclared && ts_node_is_null(createBody))
    {
      // Defined out of line: only in the constructor's file will do.
      createDoc = here;
      for (const TSNode function : definitionsIn(parent(ctor)))
      {
        const TSNode name = field(field(function, "declarator"), "declarator");
        if (kind(name) == Kind::QualifiedIdentifier &&
            text(field(name, "name")) == "create" &&
            lastName(text(field(name, "scope"))) == owner)
          createBody = field(function, "body");
      }
      if (ts_node_is_null(createBody))
        return;
    }

    // What moves: each call's statement, with the comment lines above it and
    // a comment beside it.
    std::string moved;
    std::vector<std::pair<uint32_t, uint32_t>> taken;
    for (const TSNode call : calls)
    {
      const TSNode statement = parent(call);
      uint32_t from = commentsAbove(statement, lines);
      uint32_t to = end(statement);
      const TSNode beside = nextNamed(statement);
      if (kind(beside) == Kind::Comment &&
          lines.point(start(beside)).row == lines.point(to).row)
        to = end(beside);
      const bool ownLines = lines.startsLine(from) && lines.endsLine(to);
      if (ownLines)
      {
        moved += reindent(
            f.source.substr(lines.lineStart(from), to - lines.lineStart(from)),
            "\x01");
        taken.push_back({lines.lineStart(from), lines.nextLine(to)});
      }
      else
      {
        moved += "\x01" + std::string(f.source.substr(from, to - from)) + "\n";
        uint32_t after = to;
        while (after < f.source.size() && f.source[after] == ' ')
          ++after;
        taken.push_back({from, after});
      }
    }

    // Whether the constructor is left with nothing to do, and can go.
    bool empty = true;
    const uint32_t statements = namedCount(body);
    for (uint32_t i = 0; i < statements && empty; ++i)
    {
      const TSNode statement = namedChild(body, i);
      bool goes = false;
      for (const auto& [from, to] : taken)
        goes = goes || (start(statement) >= from && end(statement) <= to);
      empty = goes;
    }
    bool initialisers = false;
    const uint32_t parts = namedCount(ctor);
    for (uint32_t i = 0; i < parts; ++i)
      initialisers = initialisers ||
                     kind(namedChild(ctor, i)) == Kind::FieldInitializerList;
    const bool removable = empty && parameterCount == 0 && !initialisers &&
                           (!outOfLine || !ts_node_is_null(declaration));

    // The moved lines indented for where they go ("\x01" marks the indent).
    const auto indented = [&](const std::string& indent)
    {
      std::string out;
      for (const char c : moved)
        if (c == '\x01')
          out += indent;
        else
          out += c;
      return out;
    };

    const std::string ctorIndent(lines.indentOf(start(ctor)));
    if (!ts_node_is_null(createBody))
    {
      const Lines& createLines = *createDoc.lines;
      const uint32_t brace = start(createBody);
      if (!createLines.endsLine(brace + 1))
        return;
      std::string indent;
      if (namedCount(createBody) > 0)
        indent =
            std::string(createLines.indentOf(start(namedChild(createBody, 0))));
      else
        indent = std::string(createLines.indentOf(brace)) + "  ";
      const uint32_t at = createLines.nextLine(brace);
      fix.push_back(createDoc.edit(at, at, indented(indent)));
      if (!removable)
        for (const auto& [from, to] : taken)
          fix.push_back(here.edit(from, to, {}));
      else if (outOfLine)
      {
        fix.push_back(removeDefinition(ctor, here));
        fix.push_back(removeMember(declaration, classDoc));
      }
      else
        fix.push_back(removeMember(ctor, here));
    }
    else if (outOfLine)
    {
      const std::string definition =
          ctorIndent + "void " + std::string(scope) + "::create()\n" +
          ctorIndent + "{\n" + indented(ctorIndent + "  ") + ctorIndent + "}";
      if (removable)
      {
        fix.push_back(
            here.edit(lines.lineStart(start(ctor)), end(ctor), definition));
        fix.push_back(removeMember(declaration, classDoc));
      }
      else
      {
        for (const auto& [from, to] : taken)
          fix.push_back(here.edit(from, to, {}));
        fix.push_back(here.edit(end(ctor), end(ctor), "\n\n" + definition));
      }
      if (!addProtected(
              classNode,
              classDoc,
              memberIndent(classNode, classDoc) + "void create() override;\n",
              fix))
        return fix.clear();
    }
    else
    {
      const std::string indent = memberIndent(classNode, classDoc);
      const std::string definition = indent + "void create() override\n" +
                                     indent + "{\n" + indented(indent + "  ") +
                                     indent + "}\n";
      if (removable)
        fix.push_back(removeMember(ctor, here));
      else
        for (const auto& [from, to] : taken)
          fix.push_back(here.edit(from, to, {}));
      if (!addProtected(classNode, classDoc, definition, fix))
        return fix.clear();
    }
    mergeTouching(fix);
  }

  // The function definitions at the top level of a tree (in namespaces too).
  std::vector<TSNode> definitionsIn(TSNode root) const
  {
    while (!ts_node_is_null(parent(root)))
      root = parent(root);
    std::vector<TSNode> found;
    std::vector<TSNode> pending = {root};
    while (!pending.empty())
    {
      const TSNode node = pending.back();
      pending.pop_back();
      const uint32_t count = namedCount(node);
      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode child = namedChild(node, i);
        const Kind k = kind(child);
        if (k == Kind::FunctionDefinition)
          found.push_back(child);
        else if (k == Kind::NamespaceDefinition ||
                 std::strcmp(typeName(child), "declaration_list") == 0)
          pending.push_back(child);
      }
    }
    return found;
  }

  // The edit removing an out-of-line definition's lines, and one blank line
  // after it so two don't meet.
  TextEdit removeDefinition(TSNode definition, const Doc& doc) const
  {
    const Lines& lines = *doc.lines;
    const uint32_t from = lines.lineStart(start(definition));
    uint32_t to = lines.nextLine(end(definition));
    if (to < doc.source.size() && lines.endsLine(to))
      to = lines.nextLine(to);
    return doc.edit(from, to, {});
  }

  // Joins an insertion with an edit that starts where it's made, file by
  // file, so no two edits of one file start at one place.
  static void mergeTouching(std::vector<TextEdit>& edits)
  {
    bool merged = true;
    while (merged)
    {
      merged = false;
      for (std::size_t i = 0; i < edits.size() && !merged; ++i)
        for (std::size_t j = 0; j < edits.size() && !merged; ++j)
        {
          const TextEdit& insert = edits[i];
          TextEdit& other = edits[j];
          if (i == j || insert.file != other.file ||
              insert.range.startRow != insert.range.endRow ||
              insert.range.startColumn != insert.range.endColumn ||
              insert.range.startRow != other.range.startRow ||
              insert.range.startColumn != other.range.startColumn)
            continue;
          other.text = insert.text + other.text;
          edits.erase(edits.begin() + static_cast<std::ptrdiff_t>(i));
          merged = true;
        }
    }
  }

  void run()
  {
    for (const TSNode call : f.aliveCalls)
      isAlive(call);
    for (const TSNode call : f.hasCalls)
      hasThenGet(call);
    for (const TSNode loop : f.loops)
      if (rowOf(loop, m_row))
      {
        rowLookup(loop, m_row);
        filterInLoop(loop, m_row);
        unusedRowEntity(loop, m_row);
        rowComponentCopy(loop, m_row);
      }
    for (const TSNode call : f.lookups)
      componentPointer(call);
    for (const TSNode constructor : f.constructors)
      setupInConstructor(constructor);
  }
};

} // namespace

void runEntityRules(LintFile& file)
{
  Rules rules(file);
  rules.run();
}

} // namespace sfs::lsp::lint
