#include "lints.h"

#include "lintSupport.h"
#include "projectIndex.h"

#include <sfs/codegen/attributes.h>
#include <sfs/codegen/parser.h>

#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <initializer_list>

namespace sfs::lsp
{

using codegen::Diagnostic;
using codegen::RuleId;
using codegen::SourceRange;
using codegen::TextEdit;
using Kind = Linter::Kind;
using lint::between;
using lint::isSpace;
using lint::lastName;
using lint::sameSpelling;
using lint::spelledAs;
using lint::tidy;
using lint::Wrapper;

namespace
{

// Unscoped attributes standard C++ defines.
constexpr std::array<std::string_view, 11> kStandardAttributes = {
    "assume",
    "carries_dependency",
    "deprecated",
    "fallthrough",
    "likely",
    "maybe_unused",
    "nodiscard",
    "noreturn",
    "no_unique_address",
    "unlikely",
    "indeterminate"};

// Associative containers a system mustn't key by entity.
constexpr std::array<std::string_view, 7> kEntityMaps = {"map",
                                                         "unordered_map",
                                                         "set",
                                                         "unordered_set",
                                                         "multimap",
                                                         "flat_map",
                                                         "flat_set"};

// Calls that change the registry's lists.
constexpr std::array<std::string_view, 7> kMutators = {"addComponent",
                                                       "addTag",
                                                       "removeComponent",
                                                       "createEntity",
                                                       "destroyEntity",
                                                       "addSystem",
                                                       "removeSystem"};

// Of those, the ones a hook may make on its own entity.
constexpr std::array<std::string_view, 3> kOwnEntityChanges = {
    "addComponent",
    "addTag",
    "removeComponent"};

// System methods that only run once the system is in a registry.
constexpr std::array<std::string_view, 5> kLateMethods = {"update",
                                                          "lateUpdate",
                                                          "render",
                                                          "onEntityAdded",
                                                          "onEntityRemoved"};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& list,
              std::string_view name)
{
  return std::find(list.begin(), list.end(), name) != list.end();
}

// What to undo when the walk leaves a node.
enum Leave : uint8_t
{
  LeaveSfs = 1,
  LeaveObserve = 2,
  PopClass = 4,
  PopFunction = 8,
};

} // namespace

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
  loops.clear();
  constructors.clear();
}

std::string_view LintFile::text(TSNode node) const
{
  return codegen::nodeText(source, node);
}

TSNode LintFile::field(TSNode node, std::string_view name)
{
  return ts_node_is_null(node) ? TSNode{} : codegen::childField(node, name);
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

bool LintFile::visible(std::string_view name) const
{
  return sfsDepth > 0 || usingSfs ||
         std::find(usingNames.begin(), usingNames.end(), name) !=
             usingNames.end();
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

std::string_view LintFile::declaredName(TSNode declarator) const
{
  while (!ts_node_is_null(declarator))
  {
    switch (kind(declarator))
    {
    case Kind::Identifier:
    case Kind::FieldIdentifier:
      return text(declarator);
    case Kind::InitDeclarator:
      declarator = field(declarator, "declarator");
      break;
    case Kind::ReferenceDeclarator:
    case Kind::PointerDeclarator:
    {
      const uint32_t count = ts_node_named_child_count(declarator);
      declarator =
          count > 0 ? ts_node_named_child(declarator, count - 1) : TSNode{};
      break;
    }
    default:
      return {};
    }
  }
  return {};
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

std::string_view LintFile::calleeName(TSNode call,
                                      TSNode& templateArguments) const
{
  templateArguments = TSNode{};
  TSNode function = field(call, "function");
  if (kind(function) == Kind::FieldExpression)
    function = field(function, "field");
  if (ts_node_is_null(function))
    return {};
  const Kind k = kind(function);
  if (k == Kind::TemplateFunction || k == Kind::TemplateMethod)
  {
    templateArguments = field(function, "arguments");
    return text(field(function, "name"));
  }
  if (k == Kind::QualifiedIdentifier)
    return {};
  return text(function);
}

TSNode LintFile::receiverOf(TSNode call) const
{
  const TSNode function = field(call, "function");
  return kind(function) == Kind::FieldExpression ? field(function, "argument")
                                                 : TSNode{};
}

bool LintFile::isSystemClass(std::string_view name) const
{
  const FileClass* found = fileClass(name);
  return (found && found->system) || index->isSystem(name);
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

std::string_view LintFile::ownerAt(TSNode node) const
{
  for (TSNode at = ts_node_parent(node); !ts_node_is_null(at);
       at = ts_node_parent(at))
  {
    const Kind k = kind(at);
    if (k == Kind::ClassSpecifier)
      return text(field(at, "name"));
    if (k != Kind::FunctionDefinition)
      continue;
    TSNode declarator = field(at, "declarator");
    while (!ts_node_is_null(declarator) &&
           kind(declarator) != Kind::FunctionDeclarator)
      declarator = field(declarator, "declarator");
    TSNode name = field(declarator, "declarator");
    std::string_view owner;
    while (kind(name) == Kind::QualifiedIdentifier)
    {
      owner = text(field(name, "scope"));
      name = field(name, "name");
    }
    if (!owner.empty())
      return owner;
  }
  return {};
}

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
                          : last == "Excluding" ? Wrapper::Excluding
                          : last == "Query"     ? Wrapper::Query
                                                : Wrapper::None;
  if (wrapper == Wrapper::None || !isSfsNameAt(at, name, last))
    return Wrapper::None;
  inner = spelled.substr(open + 1, spelled.size() - open - 2);
  return wrapper;
}

} // namespace lint

// One run's walk, kept between runs so its containers keep their capacity.
struct Linter::Walk : lint::LintFile
{
  enum class Role : uint8_t
  {
    Required,
    Any,
    Excluded,
  };

  // One component of a term list, and how its fix removes it. A term from a
  // query alias is reported on the alias's name and has no fix.
  struct Term
  {
    TSNode node;
    std::string_view spelling;
    Role role;
    TSNode removal; // the node the fix removes: from its list, or a statement
    bool statement; // `removal` is a statement, removed with its line
  };

  // An Any group of a term list: its members are terms[first, first + count).
  struct Group
  {
    TSNode node;
    std::string_view spelling;
    TSNode removal;
    bool statement;
    std::size_t first;
    std::size_t count;
  };

  struct ClassScope
  {
    std::string_view name;
    bool system;
  };

  struct FunctionScope
  {
    std::string_view
        owner; // the class it's a method of; empty for a free function
    std::string_view method; // its name
    bool system;             // a method of a System class
    std::string_view entity; // its first parameter: a hook's own entity
    std::size_t bindings, candidates,
        assigned; // where its own-entity state starts
  };

  // A reference or pointer bound to a component of an entity variable.
  struct Binding
  {
    std::string_view variable;
    std::string_view entity;
    TSNode component;
  };

  // `v.f = E` with v bound to a component of E: an own-entity store, unless E
  // is assigned or declared again in the same function.
  struct Candidate
  {
    TSNode node;
    std::string_view entity;
    TSNode component;
  };

  std::vector<ClassScope> classes;
  std::vector<FunctionScope> functions;
  std::vector<std::string_view>
      observeEntities; // per observe() callback entered: its entity parameter
  std::vector<Binding> bindings;
  std::vector<Candidate> candidates;
  std::vector<std::string_view> assigned;
  std::vector<std::string_view> checkedSystems;
  std::vector<Term> terms;
  std::vector<Group> groups;
  std::vector<uint8_t>
      leaves; // per node being walked: what to undo on leaving it

  void reset()
  {
    resetFile();
    classes.clear();
    functions.clear();
    observeEntities.clear();
    bindings.clear();
    candidates.clear();
    assigned.clear();
    checkedSystems.clear();
    terms.clear();
    groups.clear();
    leaves.clear();
  }

  // ---------------------------------------------------------------- walking

  // One walk of the tree. What each node entered needs undoing is kept in
  // `leaves` until the walk leaves it; ERROR subtrees are skipped.
  void walk(TSNode root)
  {
    lint::walkTree(
        root,
        [&](TSNode node)
        {
          const bool skip = ts_node_is_error(node);
          leaves.push_back(skip ? 0 : enter(node));
          return skip ? lint::Next::Skip : lint::Next::Descend;
        },
        [&](TSNode)
        {
          leaveNode(leaves.back());
          leaves.pop_back();
        });
  }

  uint8_t enter(TSNode node)
  {
    switch (kind(node))
    {
    case Kind::NamespaceDefinition:
      if (!codegen::isSfsNamespace(text(field(node, "name"))))
        return 0;
      ++sfsDepth;
      return LeaveSfs;
    case Kind::UsingDeclaration:
      readUsing(node);
      return 0;
    case Kind::ClassSpecifier:
      return enterClass(node) ? PopClass : 0;
    case Kind::FunctionDefinition:
      readConstructor(node);
      return enterFunction(node) ? PopFunction : 0;
    case Kind::LambdaExpression:
      if (!isObserveCallback(node))
        return 0;
      observeEntities.push_back(
          firstParameter(field(field(node, "declarator"), "parameters")));
      return LeaveObserve;
    case Kind::CallExpression:
      readCall(node);
      return 0;
    case Kind::FieldDeclaration:
      checkPerEntityMap(node);
      return 0;
    case Kind::InitDeclarator:
      redeclared(declaredName(field(node, "declarator")));
      readBinding(node);
      return 0;
    case Kind::ForRangeLoop:
      loops.push_back(node);
      redeclared(declaredName(field(node, "declarator")));
      return 0;
    case Kind::ParameterDeclaration:
      redeclared(declaredName(field(node, "declarator")));
      return 0;
    case Kind::Declaration:
      readDeclaration(node);
      // A condition's declaration has its value itself: if (auto* v = ...).
      if (!ts_node_is_null(field(node, "value")))
        readBinding(node);
      return 0;
    case Kind::AssignmentExpression:
      readAssignment(node);
      return 0;
    case Kind::Attribute:
      checkAttribute(node);
      return 0;
    default:
      return 0;
    }
  }

  void leaveNode(uint8_t leave)
  {
    if (leave & LeaveSfs)
      --sfsDepth;
    if (leave & LeaveObserve)
      observeEntities.pop_back();
    if (leave & PopClass)
      classes.pop_back();
    if (leave & PopFunction)
      leaveFunction();
  }

  void readUsing(TSNode node)
  {
    const bool usingNamespace =
        spelledAs(text(node), {"using", "namespace"}, false);
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      if (usingNamespace)
        usingSfs = usingSfs || spelledAs(text(child), {"sfs"}) ||
                   spelledAs(text(child), {"::sfs"});
      else if (kind(child) == Kind::QualifiedIdentifier &&
               text(field(child, "scope")) == "sfs")
        usingNames.push_back(text(field(child, "name")));
    }
  }

  bool enterClass(TSNode node)
  {
    if (ts_node_is_null(field(node, "body")))
      return false;
    ClassScope scope{text(field(node, "name")), false};
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode clause = ts_node_named_child(node, i);
      if (kind(clause) != Kind::BaseClassClause)
        continue;
      const uint32_t bases = ts_node_named_child_count(clause);
      for (uint32_t b = 0; b < bases; ++b)
        scope.system =
            scope.system ||
            isSfsName(text(ts_node_named_child(clause, b)), "System");
    }
    fileClasses.push_back({scope.name, node, scope.system});
    classes.push_back(scope);
    return true;
  }

  // A constructor's definition, in its class or out of line (X::X), for
  // setup-in-constructor; its class is checked once the walk is done.
  void readConstructor(TSNode node)
  {
    TSNode declarator = field(node, "declarator");
    if (kind(declarator) != Kind::FunctionDeclarator)
      return;
    TSNode name = field(declarator, "declarator");
    std::string_view owner;
    while (kind(name) == Kind::QualifiedIdentifier)
    {
      owner = text(field(name, "scope"));
      name = field(name, "name");
    }
    if (kind(name) != Kind::Identifier)
      return;
    if (owner.empty())
      owner = inClassBody(node) && !classes.empty() ? classes.back().name
                                                    : std::string_view{};
    if (!owner.empty() && text(name) == lastName(owner))
      constructors.push_back(node);
  }

  bool enterFunction(TSNode node)
  {
    if (ts_node_is_null(field(node, "body")))
      return false;
    TSNode declarator = field(node, "declarator");
    while (!ts_node_is_null(declarator) &&
           kind(declarator) != Kind::FunctionDeclarator)
      declarator = field(declarator, "declarator");
    FunctionScope scope{
        {}, {}, false, {}, bindings.size(), candidates.size(), assigned.size()};
    if (!ts_node_is_null(declarator))
    {
      scope.entity = firstParameter(field(declarator, "parameters"));
      TSNode name = field(declarator, "declarator");
      if (kind(name) == Kind::QualifiedIdentifier)
      {
        // X::f, or ns::X::f: the class is the last scope.
        while (kind(name) == Kind::QualifiedIdentifier)
        {
          scope.owner = text(field(name, "scope"));
          name = field(name, "name");
        }
        scope.method = text(name);
        scope.system = isSystemClass(scope.owner);
      }
      else if (!ts_node_is_null(name))
      {
        scope.method = text(name);
        if (inClassBody(node) && !classes.empty())
        {
          scope.owner = classes.back().name;
          scope.system = classes.back().system;
        }
      }
    }
    functions.push_back(scope);
    return true;
  }

  // Whether a function definition is written in a class body (perhaps as a
  // template).
  bool inClassBody(TSNode node) const
  {
    TSNode parent = ts_node_parent(node);
    while (!ts_node_is_null(parent) &&
           std::strcmp(ts_node_type(parent), "template_declaration") == 0)
      parent = ts_node_parent(parent);
    return !ts_node_is_null(parent) &&
           std::strcmp(ts_node_type(parent), "field_declaration_list") == 0;
  }

  void leaveFunction()
  {
    const FunctionScope& scope = functions.back();
    const auto firstAssigned =
        assigned.begin() + static_cast<std::ptrdiff_t>(scope.assigned);
    for (std::size_t i = scope.candidates; i < candidates.size(); ++i)
    {
      const Candidate& candidate = candidates[i];
      if (std::find(firstAssigned, assigned.end(), candidate.entity) !=
          assigned.end())
        continue;
      report(RuleId::OwnEntity,
             candidate.node,
             "A component should not store the entity it's on, and " +
                 tidy(text(candidate.component)) + " is given it here (" +
                 tidy(text(candidate.node)) +
                 "). Drop the field, and use the entity the system already "
                 "has when it reads the component.");
    }
    bindings.resize(scope.bindings);
    candidates.resize(scope.candidates);
    assigned.resize(scope.assigned);
    functions.pop_back();
  }

  bool isObserveCallback(TSNode lambda) const
  {
    const TSNode arguments = ts_node_parent(lambda);
    if (kind(arguments) != Kind::ArgumentList)
      return false;
    const TSNode call = ts_node_parent(arguments);
    TSNode templateArguments;
    return kind(call) == Kind::CallExpression &&
           calleeName(call, templateArguments) == "observe";
  }

  // Whether a change call is made on `entity`: `entity.f<T>()`,
  // `sfs::Entity(entity).f<T>()` or `registry->f<T>(entity)`.
  bool changesEntity(TSNode call, std::string_view entity) const
  {
    if (entity.empty())
      return false;
    const TSNode receiver = receiverOf(call);
    if (kind(receiver) == Kind::Identifier && text(receiver) == entity)
      return true;
    if (kind(receiver) == Kind::CallExpression &&
        isSfsName(text(field(receiver, "function")), "Entity"))
    {
      const TSNode arguments = field(receiver, "arguments");
      if (ts_node_named_child_count(arguments) == 1 &&
          text(ts_node_named_child(arguments, 0)) == entity)
        return true;
    }
    const TSNode arguments = field(call, "arguments");
    return !ts_node_is_null(arguments) &&
           ts_node_named_child_count(arguments) > 0 &&
           kind(ts_node_named_child(arguments, 0)) == Kind::Identifier &&
           text(ts_node_named_child(arguments, 0)) == entity;
  }

  // --------------------------------------------------------------- calls

  void readCall(TSNode call)
  {
    TSNode arguments;
    const std::string_view name = calleeName(call, arguments);
    if (name.empty())
      return;
    const FunctionScope* function =
        functions.empty() ? nullptr : &functions.back();
    const bool hasArguments = !ts_node_is_null(arguments);
    const bool inObserve = !observeEntities.empty();

    if (name == "hasComponent" && hasArguments)
      hasCalls.push_back(call);
    else if (name == "getComponent" && hasArguments)
      lookups.push_back(call);
    else if (name == "isAlive" && !hasArguments)
      aliveCalls.push_back(call);

    if ((name == "query" || name == "observe") && hasArguments)
    {
      checkTermList(arguments, isRegistryCall(call));
      return;
    }

    if (name == "registerComponent" && hasArguments)
    {
      const bool late = inObserve || (function && function->system &&
                                      contains(kLateMethods, function->method));
      if (late)
        report(RuleId::LateRegister,
               call,
               "This registerComponent() is ignored, because the system is "
               "already in a registry. Register components in create().");
      else if (function && function->system &&
               (function->method == function->owner ||
                function->method == "create"))
      {
        const TSNode statement = ts_node_parent(call);
        registrations.push_back({function->owner,
                                 ts_node_named_child(arguments, 0),
                                 kind(statement) == Kind::ExpressionStatement
                                     ? statement
                                     : TSNode{}});
      }
      return;
    }

    if (contains(kMutators, name))
    {
      const bool hook = function && function->system &&
                        (function->method == "onEntityAdded" ||
                         function->method == "onEntityRemoved");
      if (!inObserve && !hook)
        return;
      // A hook may add components to its own entity, or remove them.
      const std::string_view entity =
          inObserve ? observeEntities.back() : function->entity;
      if (contains(kOwnEntityChanges, name) && changesEntity(call, entity))
        return;
      report(RuleId::HookMutates,
             call,
             (inObserve ? std::string("This observe() callback")
                        : "This " + std::string(function->method) + "()") +
                 " runs while the lists are being updated, so it should "
                 "change only its own entity's components. Change other "
                 "entities, and create or destroy entities and systems, in "
                 "update().");
      return;
    }

    if ((name == "getSystem" || name == "tryGetSystem" ||
         name == "hasSystem") &&
        hasArguments && function && function->system)
    {
      const std::string_view read =
          lastName(text(ts_node_named_child(arguments, 0)));
      if (read != function->owner && index->isSystem(read))
        report(RuleId::SystemReadsSystem,
               call,
               "Systems should not read other systems. Put what " +
                   std::string(function->owner) + " needs from " +
                   std::string(read) + " on a component, which " +
                   std::string(read) + " writes and " +
                   std::string(function->owner) + " reads, or have " +
                   std::string(function->owner) +
                   " write a request field there that " + std::string(read) +
                   " fills in.");
    }
  }

  // Whether a query or observe call is the registry's: made inside a system
  // (no receiver, or `this`), or on something named like the registry or the
  // scene. Another library's query<...>() takes other arguments.
  bool isRegistryCall(TSNode call) const
  {
    const TSNode receiver = receiverOf(call);
    if (ts_node_is_null(receiver) || kind(receiver) == Kind::This)
      return true;
    std::string_view spelled = text(receiver);
    while (!spelled.empty() && isSpace(spelled.back()))
      spelled.remove_suffix(1);
    return spelled.ends_with("registry") || spelled.ends_with("Registry") ||
           spelled.ends_with("scene()");
  }

  // ------------------------------------------------------------ term lists

  // The SFS wrapper a template argument is written in, and the wrapper's
  // arguments. An argument may parse as a type or, ambiguously, as an
  // expression (`Any<A, B>` as a template function).
  Wrapper wrapperOf(TSNode descriptor, TSNode& inner) const
  {
    inner = TSNode{};
    TSNode type = kind(descriptor) == Kind::TypeDescriptor
                      ? field(descriptor, "type")
                      : descriptor;
    bool qualified = false;
    if (kind(type) == Kind::QualifiedIdentifier)
    {
      if (text(field(type, "scope")) != "sfs")
        return Wrapper::None;
      qualified = true;
      type = field(type, "name");
    }
    if (kind(type) != Kind::TemplateType &&
        kind(type) != Kind::TemplateFunction)
      return Wrapper::None;
    const std::string_view name = text(field(type, "name"));
    const Wrapper wrapper = name == "Any"         ? Wrapper::Any
                            : name == "Excluding" ? Wrapper::Excluding
                            : name == "Query"     ? Wrapper::Query
                                                  : Wrapper::None;
    if (wrapper == Wrapper::None || (!qualified && !visible(name)))
      return Wrapper::None;
    inner = field(type, "arguments");
    return wrapper;
  }

  // Reads one written term into terms/groups. False when the list holds a pack.
  bool readTerm(TSNode descriptor, TSNode removal, bool statement)
  {
    if (kind(descriptor) == Kind::ParameterPackExpansion)
      return false;
    TSNode inner;
    const Wrapper wrapper = wrapperOf(descriptor, inner);
    if (wrapper == Wrapper::None)
    {
      if (const QueryAlias* named =
              alias(ownerAt(descriptor), lint::trimmed(text(descriptor))))
        return readAlias(*named, descriptor, 0);
      terms.push_back(
          {descriptor, text(descriptor), Role::Required, removal, statement});
      return true;
    }
    const uint32_t count =
        ts_node_is_null(inner) ? 0 : ts_node_named_child_count(inner);
    bool pack = false;
    if (wrapper == Wrapper::Query)
    {
      // Its terms, as if they were written in its place.
      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode member = ts_node_named_child(inner, i);
        pack = !readTerm(member, member, false) || pack;
      }
      return !pack;
    }
    if (wrapper == Wrapper::Any)
    {
      groups.push_back(
          {descriptor, text(descriptor), removal, statement, terms.size(), 0});
      for (uint32_t i = 0; i < count; ++i)
      {
        const TSNode member = ts_node_named_child(inner, i);
        if (kind(member) == Kind::ParameterPackExpansion)
        {
          pack = true;
          continue;
        }
        terms.push_back({member, text(member), Role::Any, member, false});
        ++groups.back().count;
      }
      if (count == 1 && !pack)
      {
        const TSNode member = ts_node_named_child(inner, 0);
        report(RuleId::SingleAny,
               descriptor,
               "An sfs::Any group of one component is just that component. "
               "Write " +
                   tidy(text(member)) + " instead of " +
                   tidy(text(descriptor)) + ".",
               "Replace with " + tidy(text(member)),
               {{codegen::rangeOf(descriptor), std::string(text(member))}});
      }
      return !pack;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode member = ts_node_named_child(inner, i);
      if (kind(member) == Kind::ParameterPackExpansion)
      {
        pack = true;
        continue;
      }
      // An Excluding of one component goes as a whole.
      if (count == 1)
        terms.push_back(
            {member, text(member), Role::Excluded, removal, statement});
      else
        terms.push_back({member, text(member), Role::Excluded, member, false});
    }
    return !pack;
  }

  // Reads the terms a query alias stands for. They're reported on `at`, where
  // the alias is named, and have no fixes (they're written elsewhere). False
  // when they hold a pack, or aliases nest too deep to be real.
  bool readAlias(const QueryAlias& named, TSNode at, int depth)
  {
    if (depth > 8)
      return false;
    for (const std::string& term : named.terms)
      if (!readAliasTerm(term, at, depth))
        return false;
    return true;
  }

  bool readAliasTerm(std::string_view term, TSNode at, int depth)
  {
    if (term.ends_with("..."))
      return false;
    std::string_view inner;
    switch (wrapperOfText(at, term, inner))
    {
    case Wrapper::None:
      if (const QueryAlias* nested = alias(ownerAt(at), term))
        return readAlias(*nested, at, depth + 1);
      terms.push_back({at, term, Role::Required, TSNode{}, false});
      return true;
    case Wrapper::Query:
      for (const std::string_view member : lint::splitTerms(inner))
        if (!readAliasTerm(member, at, depth + 1))
          return false;
      return true;
    case Wrapper::Any:
      groups.push_back({at, term, TSNode{}, false, terms.size(), 0});
      for (const std::string_view member : lint::splitTerms(inner))
      {
        if (member.ends_with("..."))
          return false;
        terms.push_back({at, member, Role::Any, TSNode{}, false});
        ++groups.back().count;
      }
      return true;
    case Wrapper::Excluding:
      for (const std::string_view member : lint::splitTerms(inner))
      {
        if (member.ends_with("..."))
          return false;
        terms.push_back({at, member, Role::Excluded, TSNode{}, false});
      }
      return true;
    }
    return true;
  }

  void checkTermList(TSNode arguments, bool registryCall)
  {
    terms.clear();
    groups.clear();
    bool complete = true;
    const uint32_t count = ts_node_named_child_count(arguments);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode term = ts_node_named_child(arguments, i);
      complete = readTerm(term, term, false) && complete;
    }
    const bool wrapped =
        !groups.empty() || std::any_of(terms.begin(),
                                       terms.end(),
                                       [](const Term& term)
                                       { return term.role == Role::Excluded; });
    // A second Any group is a static_assert, which the compiler reports.
    if (complete && groups.size() <= 1 && (registryCall || wrapped))
      checkTerms("query");
  }

  // The registerComponent sets of the file's systems, once every one is read.
  void checkRegistrations()
  {
    for (const lint::Registration& first : registrations)
    {
      if (std::find(checkedSystems.begin(),
                    checkedSystems.end(),
                    first.system) != checkedSystems.end())
        continue;
      checkedSystems.push_back(first.system);
      terms.clear();
      groups.clear();
      bool complete = true;
      for (const lint::Registration& registration : registrations)
      {
        if (registration.system != first.system ||
            ts_node_is_null(registration.argument))
          continue;
        const std::size_t before = groups.size();
        const bool statement = !ts_node_is_null(registration.statement);
        complete =
            readTerm(registration.argument,
                     statement ? registration.statement : registration.argument,
                     statement) &&
            complete;
        if (before == 1 && groups.size() == 2)
        {
          report(RuleId::SecondAny,
                 registration.argument,
                 "A system takes one sfs::Any group, so this second one is "
                 "ignored at run time. List every alternative in one group.");
          // The run time ignores it, and so do the other checks.
          terms.resize(groups.back().first);
          groups.pop_back();
        }
      }
      if (complete)
        checkTerms("system's list");
    }
  }

  // Whether terms[0, before) has `term`'s component in `role`.
  bool listed(Role role, const Term& term, std::size_t before) const
  {
    for (std::size_t i = 0; i < before && i < terms.size(); ++i)
      if (terms[i].role == role &&
          sameSpelling(terms[i].spelling, term.spelling))
        return true;
    return false;
  }

  // Reports `rule` on `at`, with the fix that removes `node`; with no fix when
  // `node` is null (a term an alias stands for).
  void reportRemovable(RuleId rule,
                       TSNode at,
                       std::string message,
                       std::string fixTitle,
                       TSNode node,
                       bool statement)
  {
    if (ts_node_is_null(node))
      report(rule, at, std::move(message));
    else
      report(rule,
             at,
             std::move(message),
             std::move(fixTitle),
             {removal(node, statement)});
  }

  // The set rules over terms/groups: duplicate-term, any-always-met,
  // excluded-any-member and matches-nothing.
  void checkTerms(std::string_view what)
  {
    for (std::size_t i = 0; i < terms.size(); ++i)
    {
      const Term& term = terms[i];
      if (term.role == Role::Any)
        continue; // within its group, below
      if (listed(term.role, term, i))
        reportRemovable(RuleId::DuplicateTerm,
                        term.node,
                        "This list names " + tidy(term.spelling) +
                            " twice. Remove the repeat.",
                        "Remove the repeat",
                        term.removal,
                        term.statement);
    }
    for (const Group& group : groups)
    {
      const std::string written = tidy(group.spelling);
      std::size_t excludedMembers = 0;
      const Term* alwaysMet = nullptr;
      for (std::size_t i = group.first; i < group.first + group.count; ++i)
      {
        const Term& member = terms[i];
        bool repeated = false;
        for (std::size_t j = group.first; j < i; ++j)
          repeated =
              repeated || sameSpelling(terms[j].spelling, member.spelling);
        if (repeated)
          reportRemovable(RuleId::DuplicateTerm,
                          member.node,
                          "This list names " + tidy(member.spelling) +
                              " twice. Remove the repeat.",
                          "Remove the repeat",
                          member.removal,
                          false);
        if (listed(Role::Excluded, member, terms.size()))
          ++excludedMembers;
        if (!alwaysMet && listed(Role::Required, member, terms.size()))
          alwaysMet = &member;
      }
      if (alwaysMet)
        reportRemovable(RuleId::AnyAlwaysMet,
                        group.node,
                        "The group " + written + " is always met, because " +
                            tidy(alwaysMet->spelling) +
                            " is required anyway. Remove the group.",
                        "Remove " + written,
                        group.removal,
                        group.statement);
      if (group.count > 0 && excludedMembers == group.count)
        report(RuleId::MatchesNothing,
               group.node,
               "This " + std::string(what) +
                   " matches nothing, because every component of " + written +
                   " is excluded. Exclude fewer of them, or drop the group.");
      else if (excludedMembers > 0)
        for (std::size_t i = group.first; i < group.first + group.count; ++i)
          if (listed(Role::Excluded, terms[i], terms.size()))
            reportRemovable(
                RuleId::ExcludedAnyMember,
                terms[i].node,
                "The member " + tidy(terms[i].spelling) + " of " + written +
                    " is also excluded, so it never matches. Remove it from "
                    "the group.",
                "Remove " + tidy(terms[i].spelling) + " from " + written,
                terms[i].removal,
                false);
    }
    for (std::size_t i = 0; i < terms.size(); ++i)
      if (terms[i].role == Role::Required &&
          !listed(Role::Required, terms[i], i) &&
          listed(Role::Excluded, terms[i], terms.size()))
        report(RuleId::MatchesNothing,
               terms[i].node,
               "This " + std::string(what) + " matches nothing, because " +
                   tidy(terms[i].spelling) +
                   " is both required and excluded. Keep it in only one of "
                   "the two.");
  }

  // The edit that removes `node`: a term with one neighbouring comma, or a
  // statement with its line when nothing else is on it.
  TextEdit removal(TSNode node, bool statement) const
  {
    if (statement)
    {
      const uint32_t start = ts_node_start_byte(node);
      const uint32_t end = ts_node_end_byte(node);
      uint32_t lineStart = start;
      while (lineStart > 0 &&
             (source[lineStart - 1] == ' ' || source[lineStart - 1] == '\t'))
        --lineStart;
      uint32_t lineEnd = end;
      while (lineEnd < source.size() &&
             (source[lineEnd] == ' ' || source[lineEnd] == '\t' ||
              source[lineEnd] == '\r'))
        ++lineEnd;
      const TSPoint from = ts_node_start_point(node);
      const TSPoint to = ts_node_end_point(node);
      if ((lineStart == 0 || source[lineStart - 1] == '\n') &&
          (lineEnd == source.size() || source[lineEnd] == '\n'))
        return {{from.row, 0, to.row + 1, 0}, {}};
      return {between(from, to), {}};
    }
    const TSNode next = ts_node_next_named_sibling(node);
    if (!ts_node_is_null(next))
      return {
          between(ts_node_start_point(node), ts_node_start_point(next)), {}};
    const TSNode previous = ts_node_prev_named_sibling(node);
    if (!ts_node_is_null(previous))
      return {
          between(ts_node_end_point(previous), ts_node_end_point(node)), {}};
    return {codegen::rangeOf(node), {}};
  }

  // -------------------------------------------------------- system fields

  void checkPerEntityMap(TSNode node)
  {
    if (classes.empty() || !classes.back().system)
      return;
    const TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent) ||
        std::strcmp(ts_node_type(parent), "field_declaration_list") != 0)
      return;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);
      if (kind(child) == Kind::StorageClassSpecifier && text(child) == "static")
        return;
    }
    TSNode type = field(node, "type");
    if (kind(type) == Kind::QualifiedIdentifier)
      type = field(type, "name");
    if (kind(type) != Kind::TemplateType ||
        !contains(kEntityMaps, text(field(type, "name"))))
      return;
    const TSNode arguments = field(type, "arguments");
    if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0)
      return;
    const std::string_view key = text(ts_node_named_child(arguments, 0));
    if (!isSfsName(key, "Entity") &&
        !isSfsName(key, "Entity::EntityId", "Entity"))
      return;
    report(RuleId::PerEntityMap,
           node,
           "A system should not keep per-entity data in a map keyed by "
           "entity. Put the data on a component, in a hidden field.");
  }

  // ----------------------------------------------------------- own-entity

  // For `E.getComponent<C>()` or `R->getComponent<C>(E)`: E and the C node.
  bool
  componentOf(TSNode call, std::string_view& entity, TSNode& component) const
  {
    if (kind(call) != Kind::CallExpression)
      return false;
    TSNode templateArguments;
    if (calleeName(call, templateArguments) != "getComponent" ||
        ts_node_is_null(templateArguments) ||
        ts_node_named_child_count(templateArguments) != 1)
      return false;
    component = ts_node_named_child(templateArguments, 0);
    const TSNode arguments = field(call, "arguments");
    const uint32_t count =
        ts_node_is_null(arguments) ? 0 : ts_node_named_child_count(arguments);
    if (count == 1 &&
        kind(ts_node_named_child(arguments, 0)) == Kind::Identifier)
    {
      entity = text(ts_node_named_child(arguments, 0));
      return true;
    }
    const TSNode receiver = receiverOf(call);
    if (count == 0 && kind(receiver) == Kind::Identifier)
    {
      entity = text(receiver);
      return true;
    }
    return false;
  }

  const Binding* bindingOf(std::string_view variable) const
  {
    const std::size_t start = functions.empty() ? 0 : functions.back().bindings;
    for (std::size_t i = bindings.size(); i > start; --i)
      if (bindings[i - 1].variable == variable)
        return &bindings[i - 1];
    return nullptr;
  }

  // A name declared again (a parameter, a loop variable, a local) after a
  // component of an entity of that name was bound: from there the name may be
  // another entity, so its stores aren't counted.
  void redeclared(std::string_view name)
  {
    if (functions.empty() || name.empty())
      return;
    for (std::size_t i = functions.back().bindings; i < bindings.size(); ++i)
      if (bindings[i].entity == name)
      {
        assigned.push_back(name);
        return;
      }
  }

  // A declaration's plain declarators (`sfs::Entity entity;`); ones with a
  // value are init declarators, read on their own.
  void readDeclaration(TSNode node)
  {
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
      const char* fieldName = ts_node_field_name_for_named_child(node, i);
      const TSNode child = ts_node_named_child(node, i);
      if (fieldName && std::strcmp(fieldName, "declarator") == 0 &&
          kind(child) != Kind::InitDeclarator)
        redeclared(declaredName(child));
    }
  }

  // A reference or pointer bound to a component: `C& v = E.getComponent<C>()`
  // (an init declarator), or a condition's `if (C* v = E.getComponent<C>())`.
  void readBinding(TSNode node)
  {
    if (functions.empty())
      return;
    const TSNode declarator = field(node, "declarator");
    if (kind(declarator) != Kind::ReferenceDeclarator &&
        kind(declarator) != Kind::PointerDeclarator)
      return;
    const std::string_view name = declaredName(declarator);
    TSNode value = field(node, "value");
    if (name.empty() || ts_node_is_null(value))
      return;
    if (kind(value) == Kind::PointerExpression && text(value).starts_with("&"))
      value = field(value, "argument");
    std::string_view entity;
    TSNode component;
    if (componentOf(value, entity, component))
      bindings.push_back({name, entity, component});
    else if (kind(value) == Kind::Identifier)
      if (const Binding* alias = bindingOf(text(value)))
        bindings.push_back({name, alias->entity, alias->component});
  }

  void readAssignment(TSNode node)
  {
    if (functions.empty())
      return;
    const TSNode left = field(node, "left");
    const TSNode right = field(node, "right");
    if (kind(left) == Kind::Identifier)
    {
      assigned.push_back(text(left));
      return;
    }
    const TSNode op = field(node, "operator");
    if (kind(left) != Kind::FieldExpression ||
        kind(right) != Kind::Identifier ||
        (!ts_node_is_null(op) && text(op) != "="))
      return;
    const std::string_view entity = text(right);
    const TSNode receiver = field(left, "argument");
    if (kind(receiver) == Kind::Identifier)
    {
      if (const Binding* binding = bindingOf(text(receiver));
          binding && binding->entity == entity)
        candidates.push_back({node, binding->entity, binding->component});
      return;
    }
    std::string_view bound;
    TSNode component;
    if (componentOf(receiver, bound, component) && bound == entity)
      candidates.push_back({node, bound, component});
  }

  // ----------------------------------------------------------- attributes

  void checkAttribute(TSNode node)
  {
    // [[using gnu: hot]] names a vendor's attributes: not checked.
    if (spelledAs(text(ts_node_parent(node)), {"[[using"}, false))
      return;
    const TSNode nameNode = field(node, "name");
    if (ts_node_is_null(nameNode))
      return;
    const std::string_view name = text(nameNode);
    const TSNode prefix = field(node, "prefix");
    if (!ts_node_is_null(prefix))
    {
      // A table attribute under a namespace one edit from sfs: sf::hide,
      // sfx::hide.
      const std::string_view scope = text(prefix);
      if (scope != "sfs" && codegen::editDistance(scope, "sfs") <= 1 &&
          codegen::findAttribute(name))
        report(RuleId::UnknownAttribute,
               node,
               "There is no attribute " + std::string(scope) + "::" +
                   std::string(name) + ". Use sfs::" + std::string(name) +
                   " if that's what you meant.",
               "Change to sfs::" + std::string(name),
               {{codegen::rangeOf(prefix), "sfs"}});
      return;
    }
    // GCC and Clang take the standard names in their reserved spelling too:
    // __nodiscard__.
    std::string_view plain = name;
    if (plain.size() > 4 && plain.starts_with("__") && plain.ends_with("__"))
      plain = plain.substr(2, plain.size() - 4);
    if (contains(kStandardAttributes, plain))
      return;
    if (codegen::findAttribute(name))
    {
      report(RuleId::UnknownAttribute,
             node,
             "There is no attribute " + std::string(name) + ". Use sfs::" +
                 std::string(name) + " if that's what you meant.",
             "Change to sfs::" + std::string(name),
             {{codegen::rangeOf(nameNode), "sfs::" + std::string(name)}});
      return;
    }
    std::string_view closest;
    std::size_t best = 3;
    for (const std::string_view standard : kStandardAttributes)
    {
      const std::size_t distance = codegen::editDistance(name, standard);
      if (distance < best)
      {
        best = distance;
        closest = standard;
      }
    }
    if (closest.empty())
      report(RuleId::UnknownAttribute,
             node,
             "There is no standard attribute " + std::string(name) +
                 ". Remove it, or write it in its vendor's namespace.");
    else
      report(RuleId::UnknownAttribute,
             node,
             "There is no attribute " + std::string(name) + ". Use " +
                 std::string(closest) + " if that's what you meant.",
             "Change to " + std::string(closest),
             {{codegen::rangeOf(nameNode), std::string(closest)}});
  }
};

Linter::Linter() : m_walk(std::make_unique<Walk>())
{
  static const std::pair<std::string_view, Kind> kNames[] = {
      {"namespace_definition", Kind::NamespaceDefinition},
      {"class_specifier", Kind::ClassSpecifier},
      {"struct_specifier", Kind::ClassSpecifier},
      {"function_definition", Kind::FunctionDefinition},
      {"lambda_expression", Kind::LambdaExpression},
      {"call_expression", Kind::CallExpression},
      {"template_function", Kind::TemplateFunction},
      {"template_method", Kind::TemplateMethod},
      {"template_argument_list", Kind::TemplateArgumentList},
      {"template_type", Kind::TemplateType},
      {"type_descriptor", Kind::TypeDescriptor},
      {"qualified_identifier", Kind::QualifiedIdentifier},
      {"using_declaration", Kind::UsingDeclaration},
      {"field_declaration", Kind::FieldDeclaration},
      {"base_class_clause", Kind::BaseClassClause},
      {"init_declarator", Kind::InitDeclarator},
      {"reference_declarator", Kind::ReferenceDeclarator},
      {"pointer_declarator", Kind::PointerDeclarator},
      {"pointer_expression", Kind::PointerExpression},
      {"assignment_expression", Kind::AssignmentExpression},
      {"field_expression", Kind::FieldExpression},
      {"expression_statement", Kind::ExpressionStatement},
      {"parameter_pack_expansion", Kind::ParameterPackExpansion},
      {"attribute_declaration", Kind::AttributeDeclaration},
      {"attribute", Kind::Attribute},
      {"function_declarator", Kind::FunctionDeclarator},
      {"storage_class_specifier", Kind::StorageClassSpecifier},
      {"argument_list", Kind::ArgumentList},
      {"identifier", Kind::Identifier},
      {"parameter_declaration", Kind::ParameterDeclaration},
      {"for_range_loop", Kind::ForRangeLoop},
      {"declaration", Kind::Declaration},
      {"this", Kind::This},
      {"alias_declaration", Kind::AliasDeclaration},
      {"if_statement", Kind::IfStatement},
      {"while_statement", Kind::WhileStatement},
      {"for_statement", Kind::ForStatement},
      {"condition_clause", Kind::ConditionClause},
      {"init_statement", Kind::InitStatement},
      {"compound_statement", Kind::CompoundStatement},
      {"binary_expression", Kind::BinaryExpression},
      {"unary_expression", Kind::UnaryExpression},
      {"conditional_expression", Kind::ConditionalExpression},
      {"parenthesized_expression", Kind::ParenthesizedExpression},
      {"return_statement", Kind::ReturnStatement},
      {"break_statement", Kind::BreakStatement},
      {"continue_statement", Kind::ContinueStatement},
      {"field_declaration_list", Kind::FieldDeclarationList},
      {"template_declaration", Kind::TemplateDeclaration},
      {"access_specifier", Kind::AccessSpecifier},
      {"comment", Kind::Comment},
      {"structured_binding_declarator", Kind::StructuredBindingDeclarator},
      {"lambda_capture_specifier", Kind::LambdaCaptureSpecifier},
      {"lambda_capture_initializer", Kind::LambdaCaptureInitializer},
      {"lambda_default_capture", Kind::LambdaDefaultCapture},
      {"subscript_expression", Kind::SubscriptExpression},
      {"field_initializer_list", Kind::FieldInitializerList},
      {"field_identifier", Kind::FieldIdentifier},
      {"catch_clause", Kind::CatchClause},
      {"update_expression", Kind::UpdateExpression},
      {"try_statement", Kind::TryStatement},
      {"delete_method_clause", Kind::DeleteMethodClause},
      {"default_method_clause", Kind::DefaultMethodClause},
      {"optional_parameter_declaration", Kind::OptionalParameterDeclaration},
      {"translation_unit", Kind::TranslationUnit},
      {"switch_statement", Kind::SwitchStatement},
      {"destructor_name", Kind::DestructorName},
  };
  // Every symbol of one name maps to its kind, so aliased symbols match too.
  const TSLanguage* language = tree_sitter_cpp();
  const uint32_t count = ts_language_symbol_count(language);
  m_kinds.assign(count, Kind::Other);
  for (uint32_t symbol = 0; symbol < count; ++symbol)
  {
    if (ts_language_symbol_type(language, static_cast<TSSymbol>(symbol)) !=
        TSSymbolTypeRegular)
      continue;
    const std::string_view name =
        ts_language_symbol_name(language, static_cast<TSSymbol>(symbol));
    for (const auto& [known, kind] : kNames)
      if (name == known)
        m_kinds[symbol] = kind;
  }
}

Linter::~Linter() = default;

Kind Linter::kindOf(TSNode node) const
{
  if (ts_node_is_null(node))
    return Kind::Other;
  const TSSymbol symbol = ts_node_symbol(node);
  return symbol < m_kinds.size() ? m_kinds[symbol] : Kind::Other;
}

void Linter::run(std::string_view source,
                 const TSTree* tree,
                 const std::filesystem::path& path,
                 const ProjectIndex& index,
                 std::vector<Diagnostic>& out,
                 const FileReader* read)
{
  if (!tree)
    return;
  static const FileReader fromDisk = [](const std::filesystem::path& file)
  { return codegen::readFile(file); };
  Walk& walk = *m_walk;
  walk.reset();
  walk.linter = this;
  walk.source = source;
  walk.path = &path;
  walk.index = &index;
  walk.out = &out;
  walk.read = read ? read : &fromDisk;
  // Aliases first: a class's alias may be named in a method written above it.
  findQueryAliases(tree, source, path, walk.aliases);
  walk.walk(ts_tree_root_node(tree));
  walk.checkRegistrations();
  lint::runEntityRules(walk);
}

} // namespace sfs::lsp
