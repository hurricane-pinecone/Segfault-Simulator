#include "lintSupport.h"

#include "lints.h"
#include "projectIndex.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/parser.h"
#include "sfs/codegen/rules.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfs::lsp
{

using codegen::RuleId;
using codegen::SourceRange;
using codegen::TextEdit;
using Kind = Linter::Kind;

namespace lint
{

bool isSpace(char c)
{
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool spelledAs(std::string_view text,
               std::initializer_list<std::string_view> parts,
               bool whole)
{
  std::size_t at = 0;

  for (const std::string_view part : parts)
    for (const char c : part)
    {
      while (at < text.size() && isSpace(text[at]))
        ++at;

      if (at >= text.size() || text[at] != c)
        return false;

      ++at;
    }

  if (!whole)
    return true;

  while (at < text.size() && isSpace(text[at]))
    ++at;

  return at == text.size();
}

bool sameSpelling(std::string_view a, std::string_view b)
{
  return spelledAs(a, {b});
}

std::string tidy(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  bool space = false;

  for (const char c : text)
  {
    if (isSpace(c))
    {
      space = !out.empty();
      continue;
    }

    if (space)
      out += ' ';

    space = false;
    out += c;
  }

  return out;
}

std::string_view trimmed(std::string_view text)
{
  while (!text.empty() && isSpace(text.front()))
    text.remove_prefix(1);

  while (!text.empty() && isSpace(text.back()))
    text.remove_suffix(1);

  return text;
}

std::string_view lastName(std::string_view spelled)
{
  spelled = trimmed(spelled);
  const std::size_t colons = spelled.rfind("::");

  if (colons != std::string_view::npos)
    spelled.remove_prefix(colons + 2);

  return trimmed(spelled);
}

std::vector<std::string_view> splitTerms(std::string_view list)
{
  std::vector<std::string_view> items;
  int depth = 0;
  std::size_t start = 0;

  for (std::size_t i = 0; i <= list.size(); ++i)
  {
    const char c = i < list.size() ? list[i] : ',';

    if (c == '<' || c == '(' || c == '[' || c == '{')
      ++depth;
    else if (c == '>' || c == ')' || c == ']' || c == '}')
      --depth;
    else if (c == ',' && depth <= 0)
    {
      const std::string_view item = trimmed(list.substr(start, i - start));

      if (!item.empty())
        items.push_back(item);

      start = i + 1;
    }
  }

  return items;
}

SourceRange between(TSPoint start, TSPoint end)
{
  return {start.row, start.column, end.row, end.column};
}

std::string_view
calleeName(TSNode call, std::string_view source, TSNode& templateArguments)
{
  templateArguments = TSNode{};
  TSNode function = LintFile::field(call, "function");

  if (isType(function, "field_expression"))
    function = LintFile::field(function, "field");

  // x->template f<T>(): the method is inside the dependent name.
  if (isType(function, "dependent_name") && namedCount(function) == 1)
    function = namedChild(function, 0);

  if (ts_node_is_null(function) || isType(function, "qualified_identifier"))
    return {};

  if (isType(function, "template_function") ||
      isType(function, "template_method"))
  {
    templateArguments = LintFile::field(function, "arguments");
    return codegen::nodeText(source, LintFile::field(function, "name"));
  }

  return codegen::nodeText(source, function);
}

std::string_view declaredName(TSNode declarator, std::string_view source)
{
  while (!ts_node_is_null(declarator))
  {
    if (isType(declarator, "identifier") ||
        isType(declarator, "field_identifier"))
      return codegen::nodeText(source, declarator);

    if (isType(declarator, "init_declarator"))
      declarator = LintFile::field(declarator, "declarator");
    else if (isType(declarator, "reference_declarator") ||
             isType(declarator, "pointer_declarator"))
    {
      const uint32_t count = namedCount(declarator);
      declarator = count > 0 ? namedChild(declarator, count - 1) : TSNode{};
    }
    else
      return {};
  }

  return {};
}

std::string_view ownerOf(TSNode node, std::string_view source)
{
  for (TSNode at = parent(node); !ts_node_is_null(at); at = parent(at))
  {
    if (isType(at, "class_specifier") || isType(at, "struct_specifier"))
      return codegen::nodeText(source, LintFile::field(at, "name"));

    if (!isType(at, "function_definition"))
      continue;

    TSNode declarator = LintFile::field(at, "declarator");

    while (!ts_node_is_null(declarator) &&
           !isType(declarator, "function_declarator"))
      declarator = LintFile::field(declarator, "declarator");

    TSNode name = LintFile::field(declarator, "declarator");
    std::string_view owner;

    while (isType(name, "qualified_identifier"))
    {
      owner = codegen::nodeText(source, LintFile::field(name, "scope"));
      name = LintFile::field(name, "name");
    }

    if (!owner.empty())
      return lastName(owner);
  }

  return {};
}

void namespacesOf(TSNode node,
                  std::string_view source,
                  std::vector<std::string>& out)
{
  out.clear();
  // A nested namespace (`namespace a::b`) is its parts, as if each were
  // written on its own; an unnamed one is "".
  const auto add = [&](std::string_view written)
  {
    do
    {
      const std::size_t colons = written.find("::");
      std::string_view part = trimmed(written.substr(0, colons));

      if (part.starts_with("inline "))
        part = trimmed(part.substr(7));

      out.emplace_back(part);
      written = colons == std::string_view::npos ? std::string_view{}
                                                 : written.substr(colons + 2);
    } while (!written.empty());
  };
  TSNode function{};
  std::vector<TSNode> namespaces;

  for (TSNode at = parent(node); !ts_node_is_null(at); at = parent(at))
    if (isType(at, "namespace_definition"))
      namespaces.push_back(at);
    else if (ts_node_is_null(function) && isType(at, "function_definition"))
      function = at;

  for (auto at = namespaces.rbegin(); at != namespaces.rend(); ++at)
    add(codegen::nodeText(source, LintFile::field(*at, "name")));

  TSNode declarator = LintFile::field(function, "declarator");

  while (!ts_node_is_null(declarator) &&
         !isType(declarator, "function_declarator"))
    declarator = LintFile::field(declarator, "declarator");

  for (TSNode name = LintFile::field(declarator, "declarator");
       isType(name, "qualified_identifier");
       name = LintFile::field(name, "name"))
    add(codegen::nodeText(source, LintFile::field(name, "scope")));
}

TSNode memberDeclaration(TSNode classNode,
                         std::string_view name,
                         std::string_view source)
{
  const TSNode body = LintFile::field(classNode, "body");

  for (uint32_t i = 0; i < namedCount(body); ++i)
  {
    const TSNode member = namedChild(body, i);

    if (!isType(member, "field_declaration"))
      continue;

    for (uint32_t c = 0; c < namedCount(member); ++c)
    {
      const char* fieldName = ts_node_field_name_for_named_child(member, c);

      if (fieldName && std::strcmp(fieldName, "declarator") == 0 &&
          declaredName(namedChild(member, c), source) == name)
        return member;
    }
  }

  return TSNode{};
}

namespace
{

// The parameter list of a function definition's or lambda's declarator, or
// null.
TSNode parametersOf(TSNode function)
{
  TSNode declarator = LintFile::field(function, "declarator");

  while (!ts_node_is_null(declarator) &&
         !isType(declarator, "function_declarator") &&
         !isType(declarator, "abstract_function_declarator"))
    declarator = LintFile::field(declarator, "declarator");

  return LintFile::field(declarator, "parameters");
}

// The declarator of `declaration` (a declaration or a parameter's) that
// declares `name`, or null.
TSNode
declaratorOf(TSNode declaration, std::string_view name, std::string_view source)
{
  for (uint32_t i = 0; i < namedCount(declaration); ++i)
  {
    const char* fieldName = ts_node_field_name_for_named_child(declaration, i);
    const TSNode child = namedChild(declaration, i);

    if (fieldName && std::strcmp(fieldName, "declarator") == 0 &&
        declaredName(child, source) == name)
      return child;
  }

  return TSNode{};
}

} // namespace

const QueryAlias* LintFile::alias(std::string_view owner,
                                  std::string_view name) const
{
  for (const std::string_view scope : {owner, std::string_view{}})
  {
    for (const QueryAlias& found : aliases)
      if (found.owner == scope && found.name == name)
        return &found;

    if (scope.empty())
      break;
  }

  return index->findAlias(owner, name);
}

std::string_view LintFile::calleeName(TSNode call,
                                      TSNode& templateArguments) const
{
  return lint::calleeName(call, source, templateArguments);
}

std::string_view LintFile::declaredName(TSNode declarator) const
{
  return lint::declaredName(declarator, source);
}

TSNode LintFile::field(TSNode node, std::string_view name)
{
  return ts_node_is_null(node) ? TSNode{} : codegen::childField(node, name);
}

const FileClass* LintFile::fileClass(std::string_view name) const
{
  if (name.empty())
    return nullptr;

  for (const FileClass& found : fileClasses)
    if (found.name == name)
      return &found;

  return nullptr;
}

std::string_view LintFile::firstParameter(TSNode parameters) const
{
  if (ts_node_is_null(parameters))
    return {};

  const uint32_t count = ts_node_named_child_count(parameters);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode parameter = ts_node_named_child(parameters, i);

    if (kind(parameter) == Kind::ParameterDeclaration)
      return declaredName(field(parameter, "declarator"));
  }

  return {};
}

bool LintFile::isServiceClass(std::string_view name) const
{
  const FileClass* found = fileClass(name);
  return (found && found->service) || index->isService(name);
}

bool LintFile::isSfsName(std::string_view spelled,
                         std::string_view name,
                         std::string_view visibleAs) const
{
  return spelledAs(spelled, {"sfs::", name}) ||
         (spelledAs(spelled, {name}) &&
          visible(visibleAs.empty() ? name : visibleAs));
}

bool LintFile::isSfsNameAt(TSNode at,
                           std::string_view spelled,
                           std::string_view name) const
{
  if (spelledAs(spelled, {"sfs::", name}))
    return true;

  if (!spelledAs(spelled, {name}))
    return false;

  if (usingSfs ||
      std::find(usingNames.begin(), usingNames.end(), name) != usingNames.end())
    return true;

  for (TSNode node = at; !ts_node_is_null(node); node = ts_node_parent(node))
    if (kind(node) == Kind::NamespaceDefinition &&
        codegen::isSfsNamespace(text(field(node, "name"))))
      return true;

  return false;
}

bool LintFile::isSystemClass(std::string_view name) const
{
  const FileClass* found = fileClass(name);
  return (found && found->system) || index->isSystem(name);
}

std::string_view LintFile::ownerAt(TSNode node) const
{
  return ownerOf(node, source);
}

TSNode LintFile::receiverOf(TSNode call) const
{
  const TSNode function = field(call, "function");
  return kind(function) == Kind::FieldExpression ? field(function, "argument")
                                                 : TSNode{};
}

void LintFile::report(RuleId rule,
                      TSNode node,
                      std::string message,
                      std::string fixTitle,
                      std::vector<TextEdit> fix)
{
  out->push_back({rule,
                  *path,
                  codegen::rangeOf(node),
                  std::move(message),
                  std::move(fixTitle),
                  std::move(fix)});
}

void LintFile::resetFile()
{
  usingSfs = false;
  sfsDepth = 0;
  usingNames.clear();
  fileClasses.clear();
  registrations.clear();
  aliases.clear();
  hasCalls.clear();
  aliveCalls.clear();
  lookups.clear();
  sceneDataCalls.clear();
  loops.clear();
  constructors.clear();
}

std::string_view LintFile::text(TSNode node) const
{
  return codegen::nodeText(source, node);
}

bool LintFile::visible(std::string_view name) const
{
  return sfsDepth > 0 || usingSfs ||
         std::find(usingNames.begin(), usingNames.end(), name) !=
             usingNames.end();
}

Wrapper LintFile::wrapperOfText(TSNode at,
                                std::string_view spelled,
                                std::string_view& inner) const
{
  inner = {};
  spelled = trimmed(spelled);
  const std::size_t open = spelled.find('<');

  if (open == std::string_view::npos || !spelled.ends_with('>'))
    return Wrapper::None;

  const std::string_view name = trimmed(spelled.substr(0, open));
  const std::string_view last = lastName(name);
  const Wrapper wrapper = last == "Any"         ? Wrapper::Any
                          : last == "With"      ? Wrapper::With
                          : last == "Excluding" ? Wrapper::Excluding
                          : last == "Query"     ? Wrapper::Query
                                                : Wrapper::None;

  if (wrapper == Wrapper::None || !isSfsNameAt(at, name, last))
    return Wrapper::None;

  inner = spelled.substr(open + 1, spelled.size() - open - 2);
  return wrapper;
}

TSNode
localDeclarator(TSNode at, std::string_view name, std::string_view source)
{
  const uint32_t limit = start(at);

  for (TSNode child = at, scope = parent(at); !ts_node_is_null(scope);
       child = scope, scope = parent(scope))
  {
    if (isType(scope, "function_definition") ||
        isType(scope, "lambda_expression"))
    {
      const TSNode parameters = parametersOf(scope);

      for (uint32_t i = 0; i < namedCount(parameters); ++i)
        if (const TSNode found =
                declaratorOf(namedChild(parameters, i), name, source);
            !ts_node_is_null(found))
          return found;

      if (isType(scope, "function_definition"))
        return TSNode{};

      continue;
    }

    if (isType(scope, "for_range_loop") &&
        same(LintFile::field(scope, "body"), child) &&
        declaredName(LintFile::field(scope, "declarator"), source) == name)
      return LintFile::field(scope, "declarator");

    if (isType(scope, "field_declaration_list") ||
        isType(scope, "translation_unit"))
      return TSNode{};

    // The declarations written before `at` in this scope: a block's
    // statements, a statement's init or condition. The latest one wins.
    TSNode found{};

    for (uint32_t i = 0; i < namedCount(scope); ++i)
    {
      const TSNode item = namedChild(scope, i);

      if (start(item) >= limit || same(item, child))
        break;

      if (isType(item, "declaration"))
      {
        if (const TSNode d = declaratorOf(item, name, source);
            !ts_node_is_null(d))
          found = d;
      }
      else if (isType(item, "condition_clause") ||
               isType(item, "init_statement"))
        for (uint32_t c = 0; c < namedCount(item); ++c)
          if (isType(namedChild(item, c), "declaration"))
            if (const TSNode d =
                    declaratorOf(namedChild(item, c), name, source);
                !ts_node_is_null(d))
              found = d;
    }

    if (!ts_node_is_null(found))
      return found;
  }

  return TSNode{};
}

std::string_view madeType(TSNode value, std::string_view source)
{
  if (isType(value, "compound_literal_expression"))
    return codegen::nodeText(source, LintFile::field(value, "type"));

  if (isType(value, "call_expression"))
  {
    const TSNode function = LintFile::field(value, "function");

    if (isType(function, "identifier") ||
        isType(function, "qualified_identifier"))
      return codegen::nodeText(source, function);
  }

  return {};
}

std::string_view
localTypeOf(TSNode at, std::string_view name, std::string_view source)
{
  const TSNode declarator = localDeclarator(at, name, source);

  if (ts_node_is_null(declarator))
    return {};

  const std::string_view type = trimmed(
      codegen::nodeText(source, LintFile::field(parent(declarator), "type")));

  if (type != "auto")
    return type;

  return isType(declarator, "init_declarator")
             ? madeType(LintFile::field(declarator, "value"), source)
             : std::string_view{};
}

} // namespace lint

} // namespace sfs::lsp
