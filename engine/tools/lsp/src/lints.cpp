#include "lints.h"

#include "lintSupport.h"
#include "projectIndex.h"
#include "sfs/codegen/attributes.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/parser.h"
#include "sfs/codegen/rules.h"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

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
using lint::namedChild;
using lint::namedCount;
using lint::same;
using lint::sameSpelling;
using lint::spelledAs;
using lint::tidy;
using lint::Wrapper;

namespace
{

// Unscoped attributes standard C++ defines.
constexpr std::array<std::string_view, 11> STANDARD_ATTRIBUTES = {
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
constexpr std::array<std::string_view, 7> ENTITY_MAPS = {"map",
                                                         "unordered_map",
                                                         "set",
                                                         "unordered_set",
                                                         "multimap",
                                                         "flat_map",
                                                         "flat_set"};

// Calls that change the registry's lists.
constexpr std::array<std::string_view, 7> MUTATORS = {"addComponent",
                                                      "addTag",
                                                      "removeComponent",
                                                      "createEntity",
                                                      "destroyEntity",
                                                      "addSystem",
                                                      "removeSystem"};

// Calls that look a system up, which game code never makes.
constexpr std::array<std::string_view, 5> SYSTEM_LOOKUPS = {"getSystem",
                                                            "tryGetSystem",
                                                            "hasSystem",
                                                            "forEachSystem",
                                                            "renderSystem"};

// The engine's facility classes, in sfs::facility.
constexpr std::array<std::string_view, 9> FACILITIES = {"Animation",
                                                        "Assets",
                                                        "Cameras",
                                                        "DebugDraw",
                                                        "Graphics",
                                                        "Meshes",
                                                        "Navigation",
                                                        "Physics3D",
                                                        "Prefabs"};

// Of those, the ones a hook may make on its own entity.
constexpr std::array<std::string_view, 3> OWN_ENTITY_CHANGES = {
    "addComponent",
    "addTag",
    "removeComponent"};

// Calls that keep what they're given in the container they're made on.
constexpr std::array<std::string_view, 6> CONTAINER_ADDS =
    {"push_back", "emplace_back", "insert", "emplace", "try_emplace", "push"};

// An event consumer's and requester's handler setters: what they're given
// runs later, before the owner's update.
constexpr std::array<std::string_view, 10> HANDLER_SETTERS = {"onEvent",
                                                              "onEvents",
                                                              "onLatest",
                                                              "onLatestEvents",
                                                              "onResponse",
                                                              "onResponses",
                                                              "onProgress",
                                                              "onProgresses",
                                                              "onFailure",
                                                              "onFailures"};

// Game object methods that run once the object is in a scene.
constexpr std::array<std::string_view, 11> OBJECT_LATE_METHODS = {
    "onUpdate",
    "onDestroy",
    "onProcessInput",
    "onCollisionEnter",
    "onCollisionExit",
    "onTriggerEnter",
    "onTriggerExit",
    "onCollisionEnter2D",
    "onCollisionExit2D",
    "onTriggerEnter2D",
    "onTriggerExit2D"};

// System methods that only run once the system is in a registry: a service
// has none of them.
constexpr std::array<std::string_view, 5> LATE_METHODS = {"update",
                                                          "lateUpdate",
                                                          "render",
                                                          "onEntityAdded",
                                                          "onEntityRemoved"};

// What to undo when the walk leaves a node.
enum Leave : uint8_t
{
  LeaveSfs = 1,
  LeaveObserve = 2,
  PopClass = 4,
  PopFunction = 8,
};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& list,
              std::string_view name)
{
  return std::find(list.begin(), list.end(), name) != list.end();
}

} // namespace

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
    bool with = false; // a With member: required, but bound by no row
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
    bool component;
    bool service;
  };

  struct FunctionScope
  {
    std::string_view
        owner; // the class it's a method of; empty for a free function
    std::string_view method; // its name
    bool system;             // a method of a System class
    std::string_view entity; // its first parameter: a hook's own entity
    std::size_t bindings, candidates,
        assigned;         // where its own-entity state starts
    bool service = false; // a method of a Service class
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
  // `using namespace sfs::facility;` seen, and the names that stand for a
  // facility here (`using sfs::facility::X;`, aliases), for keeps-facility.
  bool usingFacility = false;
  std::vector<std::string_view> facilityNames;
  std::vector<Term> terms;
  std::vector<Group> groups;
  // The file's responses and response types, for response-sent-twice.
  std::vector<ResponseSend> responseSends;
  std::vector<ResponseType> responseTypes;
  std::vector<UsingName> usings;
  // The file's aliases of event handle types, and the handle each names.
  std::vector<std::pair<std::string_view, std::string_view>> handleAliases;
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
    usingFacility = false;
    facilityNames.clear();
    terms.clear();
    groups.clear();
    responseSends.clear();
    responseTypes.clear();
    usings.clear();
    handleAliases.clear();
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
      noteFacilityUsing(node);
      noteUsing(node, source, *path, usings);
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
      checkMember(node);
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
      checkServiceReach(node);
      return 0;
    case Kind::QualifiedIdentifier:
      checkEngineInternal(node);
      return 0;
    case Kind::Declaration:
      readDeclaration(node);
      checkServiceReach(node);

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
    case Kind::NamespaceAliasDefinition:
      noteUsing(node, source, *path, usings);
      checkNamespaceAlias(node);
      return 0;
    case Kind::AliasDeclaration:
    case Kind::TypeDefinition:
      readHandleAlias(node);
      noteFacilityAlias(node);
      noteUsing(node, source, *path, usings);
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

  bool enterClass(TSNode node)
  {
    if (ts_node_is_null(field(node, "body")))
      return false;

    ClassScope scope{text(field(node, "name")), false, false, false};
    bool based = false;
    bool event = false;

    for (uint32_t i = 0; i < namedCount(node); ++i)
    {
      const TSNode child = namedChild(node, i);

      if (kind(child) == Kind::AttributeDeclaration)
      {
        scope.component = scope.component || marks(child, "component");
        event = event || marksEvent(child);
      }

      if (kind(child) != Kind::BaseClassClause)
        continue;

      based = true;

      for (uint32_t b = 0; b < namedCount(child); ++b)
      {
        const std::string_view base = text(namedChild(child, b));
        scope.system =
            scope.system || isSfsName(base, "System") ||
            (lastName(base) != scope.name && isSystemClass(lastName(base)));
        scope.service =
            scope.service || isSfsName(base, "Service") ||
            (lastName(base) != scope.name && isServiceClass(lastName(base)));
      }
    }

    // Codegen takes a type marked both [[sfs::component]] and an event
    // attribute as an event. It registers the baseless
    // namespace-scope class named after a script header as a component,
    // which the index has for that header.
    if (event)
      scope.component = false;
    else if (!scope.component && !based && classes.empty() && functions.empty())
    {
      const IndexEntry* entry = index->find(scope.name);
      scope.component = entry && entry->kind == codegen::TypeKind::Component &&
                        entry->header == path->lexically_normal();
    }

    fileClasses.push_back(
        {scope.name, node, scope.system, scope.component, scope.service});
    classes.push_back(scope);
    return true;
  }

  // Whether an attribute declaration on a class holds an event attribute.
  bool marksEvent(TSNode declaration) const
  {
    for (uint32_t i = 0; i < namedCount(declaration); ++i)
    {
      const TSNode attribute = namedChild(declaration, i);

      if (kind(attribute) != Kind::Attribute ||
          text(field(attribute, "prefix")) != "sfs")
        continue;

      const codegen::AttributeInfo* info =
          codegen::findAttribute(text(field(attribute, "name")));

      if (info && codegen::isEventAttribute(info->id))
        return true;
    }

    return false;
  }

  // Whether an attribute declaration on a class holds [[sfs::`name`]].
  bool marks(TSNode declaration, std::string_view name) const
  {
    for (uint32_t i = 0; i < namedCount(declaration); ++i)
    {
      const TSNode attribute = namedChild(declaration, i);

      if (kind(attribute) == Kind::Attribute &&
          text(field(attribute, "prefix")) == "sfs" &&
          text(field(attribute, "name")) == name)
        return true;
    }

    return false;
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
        scope.service = isServiceClass(scope.owner);
      }
      else if (!ts_node_is_null(name))
      {
        scope.method = text(name);

        if (inClassBody(node) && !classes.empty())
        {
          scope.owner = classes.back().name;
          scope.system = classes.back().system;
          scope.service = classes.back().service;

          if (scope.service)
            checkServiceMethod(name, scope.method);
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
    {
      readQualifiedCall(call);
      return;
    }

    const FunctionScope* function =
        functions.empty() ? nullptr : &functions.back();
    const bool hasArguments = !ts_node_is_null(arguments);
    const bool inObserve = !observeEntities.empty();

    if (name == "hasComponent" && hasArguments)
      hasCalls.push_back(call);
    else if (name == "getComponent" && hasArguments)
      lookups.push_back(call);
    else if (name == "sceneData" && hasArguments)
      sceneDataCalls.push_back(call);
    else if (name == "isAlive" && !hasArguments)
      aliveCalls.push_back(call);

    if ((name == "query" || name == "observe") && hasArguments)
    {
      if (name == "observe" && isRegistryCall(call))
        checkWithInRegister(call, arguments, false, false);

      checkTermList(arguments, isRegistryCall(call));
      return;
    }

    if ((name == "consumer" || name == "requester") && hasArguments &&
        namedCount(arguments) == 1)
    {
      const TSNode receiver = receiverOf(call);
      checkEventHandle(
          call,
          name,
          namedChild(arguments, 0),
          ts_node_is_null(receiver) || kind(receiver) == Kind::This);
      return;
    }

    if (name == "respond" || name == "progress" || name == "fail")
    {
      noteResponseSend(call, source, *path, responseSends);
      return;
    }

    if (name == "registerComponent" && hasArguments)
    {
      checkWithInRegister(call, arguments, true, true);

      if (inLateCode())
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

    if (contains(MUTATORS, name))
    {
      const bool hook = function && function->system &&
                        (function->method == "onEntityAdded" ||
                         function->method == "onEntityRemoved");

      if (!inObserve && !hook)
        return;

      // A hook may add components to its own entity, or remove them.
      const std::string_view entity =
          inObserve ? observeEntities.back() : function->entity;

      if (contains(OWN_ENTITY_CHANGES, name) && changesEntity(call, entity))
        return;

      report(RuleId::HookMutates,
             call,
             runningCode() +
                 " runs while the lists are being updated, so it should "
                 "change only its own entity's components. Change other "
                 "entities, and create or destroy entities and systems, in "
                 "update().");
      return;
    }

    if (contains(SYSTEM_LOOKUPS, name) && index->enabled() &&
        looksUpThroughHost(call))
      report(RuleId::SystemLookup,
             call,
             "Game code should not look systems up, as this " +
                 std::string(name) +
                 "() does. Use the engine's facilities (physics(), "
                 "navigation(), graphics(), ...) for engine features; read "
                 "game state from components and scene data, and send what "
                 "one part tells another as an event.");
  }

  // Whether the walk is in code that runs again and again once its object is
  // in a scene: a system's update, lateUpdate, render or hook, an observe
  // callback, or a game object's onUpdate.
  bool inLateCode() const
  {
    if (!observeEntities.empty())
      return true;

    if (functions.empty())
      return false;

    const FunctionScope& function = functions.back();

    if (function.system)
      return contains(LATE_METHODS, function.method);

    return function.method == "onUpdate" && isGameObjectClass(function.owner);
  }

  // Whether `name` is a game object class: of the file, deriving from SFS's
  // GameObject or another game object class, or one of the project's.
  bool isGameObjectClass(std::string_view name) const
  {
    return derivesFromSfs(name, {"GameObject", "ui::Layer"}, true);
  }

  // Whether `name` is a scene class: of the file, deriving from SFS's Scene
  // or StandardScene, directly or through others of the file.
  bool isSceneClass(std::string_view name) const
  {
    return derivesFromSfs(name, {"Scene", "StandardScene"}, false);
  }

  // Whether `name` derives from one of SFS's `bases`, directly or through
  // classes of the file; with `indexedObjects`, a class the project index
  // has as a game object counts too.
  bool derivesFromSfs(std::string_view name,
                      std::initializer_list<std::string_view> bases,
                      bool indexedObjects) const
  {
    // Each class is looked at once, so a cycle of bases (code being typed
    // may have one, branching too) ends the walk.
    std::vector<std::string_view> pending{name};
    std::vector<std::string_view> seen;

    while (!pending.empty())
    {
      const std::string_view at = pending.back();
      pending.pop_back();

      if (at.empty() || std::find(seen.begin(), seen.end(), at) != seen.end())
        continue;

      seen.push_back(at);
      const lint::FileClass* found = fileClass(at);

      if (!found)
      {
        if (indexedObjects && index->isGameObject(at))
          return true;

        continue;
      }

      for (uint32_t i = 0; i < namedCount(found->node); ++i)
      {
        const TSNode clause = namedChild(found->node, i);

        if (kind(clause) != Kind::BaseClassClause)
          continue;

        for (uint32_t b = 0; b < namedCount(clause); ++b)
        {
          const std::string_view base = text(namedChild(clause, b));

          for (const std::string_view sfsBase : bases)
            if (isSfsName(base, sfsBase))
              return true;

          pending.push_back(lastName(base));
        }
      }
    }

    return false;
  }

  // The code the walk is in, for a message: "This observe() callback" or
  // "This update()".
  std::string runningCode() const
  {
    if (!observeEntities.empty() || functions.empty())
      return "This observe() callback";

    return "This " + std::string(functions.back().method) + "()";
  }

  // ---------------------------------------------------------------- events

  // A call through a qualified name: `sfs::EventVerbs::consumer<E>()` is a
  // system's own consumer<E>().
  void readQualifiedCall(TSNode call)
  {
    TSNode name = field(call, "function");
    std::string_view scope;

    while (kind(name) == Kind::QualifiedIdentifier)
    {
      scope = text(field(name, "scope"));
      name = field(name, "name");
    }

    if (kind(name) != Kind::TemplateFunction || lastName(scope) != "EventVerbs")
      return;

    const std::string_view called = text(field(name, "name"));
    const TSNode arguments = field(name, "arguments");

    if ((called == "consumer" || called == "requester") &&
        namedCount(arguments) == 1)
      checkEventHandle(call, called, namedChild(arguments, 0), true);
  }

  // A consumer<E>() or requester<R>() call in a system or a game object:
  // event-handle-outside-create when a system makes it through events(), when
  // it's made in code that runs once the class is set up, or when it isn't
  // kept. `own` says it's the system's own (no receiver, `this`, or
  // EventVerbs::); otherwise its receiver must be an Events.
  void
  checkEventHandle(TSNode call, std::string_view name, TSNode type, bool own)
  {
    if (!own && !eventsReceiver(receiverOf(call)))
      return;

    std::string_view owner;
    std::string_view method;

    if (!functions.empty())
    {
      owner = functions.back().owner;
      method = functions.back().method;
    }
    else if (!classes.empty())
      owner = classes.back().name; // a member's initialiser

    // A system's and a service's handles follow the same rules.
    const bool service = isServiceClass(owner);
    const bool system = service || isSystemClass(owner);

    if (!system && (own || !isGameObjectClass(owner)))
      return;

    const std::string_view what = service ? "service" : "system";
    const std::string handle =
        std::string(name) + "<" + tidy(text(type)) + ">()";
    const std::string made = "This " + handle;
    const std::string setup = system ? "create()" : "onCreate()";

    if (system && !own)
    {
      report(RuleId::EventHandleOutsideCreate,
             call,
             made + " is made through events(), which gives a " +
                 std::string(what) +
                 "'s handle no owner, so its handlers never run. A " +
                 std::string(what) + " makes its handles with its own " +
                 handle + " in create().");
      return;
    }

    const std::string later = laterCode(call, owner, method, system);

    if (!later.empty())
    {
      report(RuleId::EventHandleOutsideCreate,
             call,
             made + " is made in " + later + ", outside " + setup + ", so " +
                 (system ? "it's refused and receives nothing"
                         : "it has no owner to run its handlers") +
                 ". Make it in " + setup + " and keep it as a member.");
      return;
    }

    if (!kept(call, owner, 0))
      report(RuleId::EventHandleOutsideCreate,
             call,
             made + " isn't kept as a member, so it's dropped when " +
                 std::string(method) + "() returns and receives nothing. " +
                 "Assign it to a data member of the " +
                 (system ? std::string(what) + "." : std::string("object.")));
  }

  // The code a call is in when that code runs once its class is set up, for
  // a message ("update()", "an event handler"); empty for create() or
  // onCreate() and the helpers they may call.
  std::string laterCode(TSNode call,
                        std::string_view owner,
                        std::string_view method,
                        bool system) const
  {
    if (inHandler(call))
      return "an event handler";

    if (!observeEntities.empty())
      return "an observe() callback";

    if (method.empty())
      return "a member's initialiser";

    if (method == lastName(owner))
      return "the constructor";

    if (method.starts_with('~'))
      return "the destructor";

    if (system ? contains(LATE_METHODS, method)
               : contains(OBJECT_LATE_METHODS, method))
      return std::string(method) + "()";

    return {};
  }

  // Whether a call is in a lambda given to a handler setter
  // (`consumer<E>().onEvent([this](...) { ... })`), which runs later.
  bool inHandler(TSNode call) const
  {
    for (TSNode at = lint::parent(call); !ts_node_is_null(at);
         at = lint::parent(at))
    {
      if (kind(at) == Kind::FunctionDefinition)
        return false;

      if (kind(at) != Kind::LambdaExpression)
        continue;

      const TSNode list = lint::parent(at);
      const TSNode setter = lint::parent(list);
      TSNode templateArguments;

      if (kind(list) == Kind::ArgumentList &&
          kind(setter) == Kind::CallExpression &&
          contains(HANDLER_SETTERS, calleeName(setter, templateArguments)))
        return true;
    }

    return false;
  }

  // Whether the handle an expression gives outlives the function making it:
  // assigned to a data member of `owner` (perhaps through a builder chain, a
  // `?:`, a cast or braces), added to a member container, returned, passed
  // to another function, or held by a local that one of those then takes.
  bool kept(TSNode value, std::string_view owner, int depth) const
  {
    TSNode at = value;

    for (;;)
    {
      const TSNode holder = lint::parent(at);
      const Kind k = kind(holder);

      if (k == Kind::ParenthesizedExpression ||
          (k == Kind::ConditionalExpression &&
           !same(field(holder, "condition"), at)) ||
          lint::isType(holder, "initializer_list") ||
          lint::isType(holder, "compound_literal_expression"))
      {
        at = holder;
        continue;
      }

      if (k == Kind::FieldExpression)
      {
        // A builder chain: requester<R>().onResponse(f).ignoreFailure().
        const TSNode outer = lint::parent(holder);

        if (!same(field(holder, "argument"), at) ||
            kind(outer) != Kind::CallExpression ||
            !same(field(outer, "function"), holder))
          return false;

        at = outer;
        continue;
      }

      if (k == Kind::ArgumentList)
      {
        const TSNode outer = lint::parent(holder);

        if (passesOn(outer))
        {
          at = outer;
          continue;
        }

        TSNode templateArguments;

        if (!contains(CONTAINER_ADDS, calleeName(outer, templateArguments)))
          return true;

        const TSNode receiver = receiverOf(outer);
        return ts_node_is_null(receiver) || memberTarget(receiver, owner);
      }

      if (k == Kind::AssignmentExpression)
        return same(field(holder, "right"), at) &&
               memberTarget(field(holder, "left"), owner);

      if (k == Kind::ReturnStatement)
        return true;

      if (k == Kind::InitDeclarator && same(field(holder, "value"), at))
        return depth < 2 && localKept(holder, owner, depth);

      return false;
    }
  }

  // Whether a call gives back the handle it's given: `std::move(h)`, or a
  // handle type's constructor, `sfs::EventConsumer<E>(h)`.
  bool passesOn(TSNode call) const
  {
    const std::string_view function = text(field(call, "function"));

    if (function == "std::move" || function == "move")
      return true;

    const std::size_t open = function.find('<');
    const std::string_view type = lastName(
        open == std::string_view::npos ? function : function.substr(0, open));
    return contains(codegen::kEventHandleTypes, type);
  }

  // Whether a local initialised with a handle hands it on later in its
  // block (kept()), or is static and so outlives the function.
  bool localKept(TSNode declarator, std::string_view owner, int depth) const
  {
    const std::string_view name = declaredName(declarator);
    const TSNode declaration = lint::parent(declarator);

    if (name.empty() || kind(declaration) != Kind::Declaration)
      return false;

    for (uint32_t i = 0; i < namedCount(declaration); ++i)
    {
      const TSNode child = namedChild(declaration, i);

      if (kind(child) == Kind::StorageClassSpecifier &&
          (text(child) == "static" || text(child) == "thread_local"))
        return true;
    }

    bool handed = false;
    const uint32_t after = lint::end(declaration);
    lint::walkTree(lint::parent(declaration),
                   [&](TSNode node)
                   {
                     if (handed)
                       return lint::Next::Stop;

                     if (lint::end(node) <= after)
                       return lint::Next::Skip;

                     if (kind(node) == Kind::Identifier && text(node) == name &&
                         same(localDeclarator(node, name), declarator) &&
                         kept(node, owner, depth + 1))
                       handed = true;

                     return lint::Next::Descend;
                   });
    return handed;
  }

  // Whether an expression names a data member of `owner`, or a part of one:
  // `m_x`, `this->x`, `x` as a member, `m_x.y`, `m_x[i]`, or a local
  // reference bound to one of those.
  bool memberTarget(TSNode node, std::string_view owner) const
  {
    switch (kind(node))
    {
    case Kind::Identifier:
    {
      const std::string_view name = text(node);
      const TSNode local = localDeclarator(node, name);

      if (!ts_node_is_null(local))
        return kind(local) == Kind::InitDeclarator &&
               kind(field(local, "declarator")) == Kind::ReferenceDeclarator &&
               memberTarget(field(local, "value"), owner);

      if (name.starts_with("m_"))
        return true;

      // A class the file doesn't declare may have it: taken as a member.
      const lint::FileClass* found = fileClass(owner);
      return !found || !ts_node_is_null(
                           lint::memberDeclaration(found->node, name, source));
    }
    case Kind::FieldExpression:
    {
      const TSNode argument = field(node, "argument");
      return kind(argument) == Kind::This || memberTarget(argument, owner);
    }
    case Kind::SubscriptExpression:
    case Kind::PointerExpression:
      return memberTarget(field(node, "argument"), owner);
    case Kind::ParenthesizedExpression:
      return namedCount(node) == 1 && memberTarget(namedChild(node, 0), owner);
    case Kind::ConditionalExpression:
      return memberTarget(field(node, "consequence"), owner) &&
             memberTarget(field(node, "alternative"), owner);
    case Kind::CallExpression:
    {
      // A member's element by reference: m_map.at(k), m_list.back(), or
      // std::get<I>(m_tuple).
      const TSNode receiver = receiverOf(node);

      if (!ts_node_is_null(receiver))
        return memberTarget(receiver, owner);

      const TSNode arguments = field(node, "arguments");
      return lastName(text(field(node, "function"))).starts_with("get<") &&
             namedCount(arguments) == 1 &&
             memberTarget(namedChild(arguments, 0), owner);
    }
    default:
      return false;
    }
  }

  // The local or parameter `name` seen at `at` (lint::localDeclarator()).
  TSNode localDeclarator(TSNode at, std::string_view name) const
  {
    return lint::localDeclarator(at, name, source);
  }

  // Whether a call's receiver is an Events: an `events()` call
  // (`registry->events()`, `scene().events()`), a local declared as one
  // (`sfs::Events& ev`) or made from one (`auto& ev = scene.events()`), or
  // another name that ends in events (`m_events`).
  bool eventsReceiver(TSNode receiver) const
  {
    if (kind(receiver) == Kind::CallExpression)
    {
      TSNode templateArguments;
      return calleeName(receiver, templateArguments) == "events" &&
             ts_node_is_null(templateArguments);
    }

    if (kind(receiver) == Kind::Identifier)
    {
      const TSNode local = localDeclarator(receiver, text(receiver));

      if (!ts_node_is_null(local))
      {
        const std::string_view type =
            lint::trimmed(text(field(lint::parent(local), "type")));

        if (lastName(type) == "Events")
          return true;

        if (kind(local) == Kind::InitDeclarator &&
            eventsReceiver(field(local, "value")))
          return true;
      }
    }

    if (kind(receiver) == Kind::FieldExpression)
      receiver = field(receiver, "field");

    if (kind(receiver) != Kind::Identifier &&
        kind(receiver) != Kind::FieldIdentifier)
      return false;

    std::string lower(text(receiver));

    for (char& c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return lower.ends_with("events");
  }

  // response-sent-twice, once the walk has seen the whole file: a response
  // a system of the file sends that another system sends too, of the file
  // or another file of the project. Each send's type is resolved once, and
  // the project's senders are looked up by type.
  void checkResponses()
  {
    if (responseSends.empty())
      return;

    const auto kindOf = [&](std::string_view name)
    {
      ResponseKind kind = index->responseKind(name);

      for (const ResponseType& type : responseTypes)
        if (type.qualifiedName == name)
          kind = std::max(
              kind,
              type.declared ? ResponseKind::Declared : ResponseKind::Listed);

      return kind;
    };
    const std::string_view fileSystem = onlySystem();
    std::vector<std::pair<std::string, std::string_view>> sent; // type, by
    sent.reserve(responseSends.size());

    for (const ResponseSend& send : responseSends)
      sent.emplace_back(
          resolveResponse(send, usings, kindOf),
          sendsFor(send.owner) ? std::string_view(send.owner) : fileSystem);

    const std::filesystem::path normal = path->lexically_normal();

    for (std::size_t i = 0; i < responseSends.size(); ++i)
    {
      const auto& [type, sender] = sent[i];

      if (type.empty() || sender.empty())
        continue;

      std::string_view other;

      for (const auto& [otherType, by] : sent)
        if (!by.empty() && by != sender && otherType == type)
        {
          other = by;
          break;
        }

      for (const ResponseSender& found : index->sendersOf(type))
      {
        if (!other.empty())
          break;

        if (*found.file != normal && found.system != sender)
          other = found.system;
      }

      if (other.empty())
        continue;

      out->push_back(
          {RuleId::ResponseSentTwice,
           *path,
           responseSends[i].range,
           "The response type " + type +
               " is sent by one system or service, the one that consumes "
               "its request, and " +
               std::string(other) +
               " already sends it. Give this one's responses a type of their "
               "own.",
           {},
           {}});
    }
  }

  // Whether a class's sends are its own for response-sent-twice: a system's
  // or a service's.
  bool sendsFor(std::string_view owner) const
  {
    return isSystemClass(owner) || isServiceClass(owner);
  }

  // The file's one system or service, which its jobs and free functions send
  // for: of its system and service classes and the owners of its sends that
  // are; empty when it has none or several.
  std::string_view onlySystem() const
  {
    std::string_view only;
    bool several = false;
    const auto consider = [&](std::string_view name)
    {
      if (only.empty())
        only = name;
      else if (only != name)
        several = true;
    };

    for (const lint::FileClass& found : fileClasses)
      if (found.system || found.service)
        consider(found.name);

    for (const ResponseSend& send : responseSends)
      if (sendsFor(send.owner))
        consider(send.owner);

    return several ? std::string_view{} : only;
  }

  // ------------------------------------------------------------ term lists

  /*
   * with-in-register on the With groups written in a registerComponent<...>()
   * or observe<...>() call's own terms (`arguments`), a Query list written
   * there included: neither binds a row, so a With group is just its
   * components. The fix unwraps it in place; registerComponent() takes one
   * term, so a group of several written as its whole term (`single`)
   * becomes one call per component, each a statement on its own line.
   */
  void checkWithInRegister(TSNode call,
                           TSNode arguments,
                           bool registering,
                           bool single)
  {
    for (uint32_t i = 0; i < namedCount(arguments); ++i)
    {
      const TSNode term = namedChild(arguments, i);
      TSNode inner;
      const Wrapper wrapper = wrapperOf(term, inner);

      if (wrapper == Wrapper::Query)
      {
        checkWithInRegister(call, inner, registering, false);
        continue;
      }

      if (wrapper != Wrapper::With || namedCount(inner) == 0)
        continue;

      std::vector<std::string_view> members;
      bool pack = false;

      for (uint32_t j = 0; j < namedCount(inner); ++j)
      {
        const TSNode member = namedChild(inner, j);
        pack = pack || kind(member) == Kind::ParameterPackExpansion;
        members.push_back(text(member));
      }

      if (pack)
        continue;

      const std::string written = tidy(text(term));
      std::string listed;

      for (const std::string_view member : members)
        listed += (listed.empty() ? "" : ", ") + tidy(member);

      const std::string message =
          std::string(registering ? "Registering" : "Observing") +
          " binds no row, so " + written + " here is just " + listed +
          ". Write the " + (members.size() == 1 ? "component" : "components") +
          " without the group.";
      const std::string title = "Write " + listed + " without the group";

      if (!single || members.size() == 1)
      {
        std::string unwrapped;

        for (const std::string_view member : members)
          unwrapped += (unwrapped.empty() ? "" : ", ") + std::string(member);

        report(RuleId::WithInRegister,
               term,
               message,
               title,
               {{codegen::rangeOf(term), std::move(unwrapped)}});
        continue;
      }

      // One registerComponent() statement per component, on lines of their
      // own at the statement's indentation.
      const TSNode statement = ts_node_parent(call);
      const uint32_t from = ts_node_start_byte(statement);
      uint32_t lineStart = from;

      while (lineStart > 0 &&
             (source[lineStart - 1] == ' ' || source[lineStart - 1] == '\t'))
        --lineStart;

      if (kind(statement) != Kind::ExpressionStatement ||
          (lineStart > 0 && source[lineStart - 1] != '\n'))
      {
        report(RuleId::WithInRegister, term, message);
        continue;
      }

      const std::string_view indent =
          source.substr(lineStart, from - lineStart);
      const std::string_view before =
          source.substr(from, ts_node_start_byte(term) - from);
      const std::string_view after =
          source.substr(ts_node_end_byte(term),
                        ts_node_end_byte(statement) - ts_node_end_byte(term));
      std::string statements;

      for (const std::string_view member : members)
      {
        if (!statements.empty())
          statements += "\n" + std::string(indent);

        statements +=
            std::string(before) + std::string(member) + std::string(after);
      }

      report(RuleId::WithInRegister,
             term,
             message,
             title,
             {{codegen::rangeOf(statement), std::move(statements)}});
    }
  }

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
                            : name == "With"      ? Wrapper::With
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

      // A With's members are required as plain components are. A group of
      // one component goes as a whole.
      const Role role =
          wrapper == Wrapper::With ? Role::Required : Role::Excluded;

      if (count == 1)
        terms.push_back({member,
                         text(member),
                         role,
                         removal,
                         statement,
                         wrapper == Wrapper::With});
      else
        terms.push_back({member,
                         text(member),
                         role,
                         member,
                         false,
                         wrapper == Wrapper::With});
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
    const Wrapper wrapper = wrapperOfText(at, term, inner);

    switch (wrapper)
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
    case Wrapper::With:
    case Wrapper::Excluding:
    {
      const bool with = wrapper == Wrapper::With;

      for (const std::string_view member : lint::splitTerms(inner))
      {
        if (member.ends_with("..."))
          return false;

        terms.push_back({at,
                         member,
                         with ? Role::Required : Role::Excluded,
                         TSNode{},
                         false,
                         with});
      }

      return true;
    }
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
        !groups.empty() ||
        std::any_of(terms.begin(),
                    terms.end(),
                    [](const Term& term)
                    { return term.role == Role::Excluded || term.with; });

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
    // A component both plain and in a With group: the With's goes, so the
    // rows keep their bindings.
    std::vector<bool> repeats(terms.size(), false);

    for (std::size_t i = 0; i < terms.size(); ++i)
    {
      const Term& term = terms[i];

      if (term.role == Role::Any || !listed(term.role, term, i))
        continue; // an Any member within its group, below

      std::size_t repeat = i;

      for (std::size_t j = 0; j < i && !term.with; ++j)
        if (terms[j].role == term.role && terms[j].with &&
            sameSpelling(terms[j].spelling, term.spelling))
        {
          repeat = j;
          break;
        }

      if (repeats[repeat])
        continue;

      repeats[repeat] = true;
      reportRemovable(RuleId::DuplicateTerm,
                      terms[repeat].node,
                      "This list names " + tidy(term.spelling) +
                          " twice. Remove the repeat.",
                      "Remove the repeat",
                      terms[repeat].removal,
                      terms[repeat].statement);
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

  // A non-static member of the class being walked: per-entity-map for a
  // system's or a service's, service-reaches-registry and
  // service-update-method for a service's, reader-on-component for a
  // component's.
  void checkMember(TSNode node)
  {
    if (classes.empty())
      return;

    const ClassScope& owner = classes.back();
    const bool object =
        !owner.system && !owner.component && !owner.service &&
        (isGameObjectClass(owner.name) || isSceneClass(owner.name));

    if (!owner.system && !owner.component && !owner.service && !object)
      return;

    const TSNode parent = ts_node_parent(node);

    if (ts_node_is_null(parent) ||
        std::strcmp(ts_node_type(parent), "field_declaration_list") != 0)
      return;

    const uint32_t count = ts_node_named_child_count(node);

    for (uint32_t i = 0; i < count; ++i)
    {
      const TSNode child = ts_node_named_child(node, i);

      // A static member outlives every scene: the worst place for one.
      if (kind(child) == Kind::StorageClassSpecifier && text(child) == "static")
      {
        checkKeepsFacility(node, owner.name);
        return;
      }
    }

    checkKeepsFacility(node, owner.name);

    if (object)
      return;

    if (owner.service)
    {
      const TSNode declarator = field(node, "declarator");

      if (kind(declarator) == Kind::FunctionDeclarator)
      {
        checkServiceMethod(field(declarator, "declarator"),
                           text(field(declarator, "declarator")));
        return;
      }

      checkServiceReach(node);
    }

    if (owner.system || owner.service)
      checkPerEntityMap(node);
    else
      checkReaderOnComponent(node);
  }

  // keeps-facility: a data member holding one of SFS's facilities (its type,
  // or a template argument of it, in sfs::facility).
  void checkKeepsFacility(TSNode node, std::string_view owner)
  {
    if (kind(field(node, "declarator")) == Kind::FunctionDeclarator ||
        !namesSfsFacility(text(field(node, "type"))))
      return;

    report(RuleId::KeepsFacility,
           node,
           "A facility is valid for the step that got it, so " +
               std::string(owner) +
               " should not keep one. Get it where you use it, and hand "
               "workers its snapshots.");
  }

  // Whether a spelled type names one of SFS's facilities anywhere in it:
  // `sfs::facility::Navigation`, `std::optional<facility::Physics3D>` where
  // SFS's names are visible, a bare name after `using namespace
  // sfs::facility;` or `using sfs::facility::X;`, or an alias of one. A
  // game's own `facility::` namespace isn't SFS's: its names aren't these.
  bool namesSfsFacility(std::string_view spelled) const
  {
    const auto identifier = [](char c)
    { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    for (std::size_t at = 0; at < spelled.size();)
    {
      if (!identifier(spelled[at]))
      {
        ++at;
        continue;
      }

      std::size_t end = at;

      while (end < spelled.size() && identifier(spelled[end]))
        ++end;

      const std::string_view word = spelled.substr(at, end - at);
      const std::string_view before = spelled.substr(0, at);
      at = end;
      const bool qualified = before.ends_with("::");

      if (!qualified &&
          std::find(facilityNames.begin(), facilityNames.end(), word) !=
              facilityNames.end())
        return true;

      if (!contains(FACILITIES, word))
        continue;

      if (before.ends_with("sfs::facility::") || (!qualified && usingFacility))
        return true;

      if (!before.ends_with("facility::"))
        continue;

      const std::string_view scope = before.substr(
          0, before.size() - std::string_view("facility::").size());

      if ((scope.empty() ||
           !(identifier(scope.back()) || scope.back() == ':')) &&
          (usingSfs || sfsDepth > 0))
        return true;
    }

    return false;
  }

  // Notes `using namespace sfs::facility;` and `using sfs::facility::X;`, so
  // keeps-facility knows the bare names.
  void noteFacilityUsing(TSNode node)
  {
    const bool directive = spelledAs(text(node), {"using", "namespace"}, false);

    for (uint32_t i = 0; i < namedCount(node); ++i)
    {
      std::string_view named = text(namedChild(node, i));

      if (named.starts_with("::"))
        named.remove_prefix(2);

      if (directive)
        usingFacility = usingFacility || named == "sfs::facility" ||
                        (named == "facility" && (usingSfs || sfsDepth > 0));
      else if (namesSfsFacility(named))
        facilityNames.push_back(lastName(named));
    }
  }

  // Notes an alias of a facility (`using Physics = sfs::facility::Physics3D;`,
  // or a typedef), so keeps-facility knows it by its new name.
  void noteFacilityAlias(TSNode node)
  {
    const bool alias = kind(node) == Kind::AliasDeclaration;
    const std::string_view type = text(field(node, "type"));
    // A typedef's declarator is the new type's name itself.
    const std::string_view name =
        alias ? text(field(node, "name")) : text(field(node, "declarator"));

    if (!name.empty() && namesSfsFacility(type))
      facilityNames.push_back(name);
  }

  // system-lookup's receivers: none (the system's or scene's own), `this`, or
  // a registry or a scene (`registry->`, `m_scene->`, `scene()`), so a game's
  // own `inventory.hasSystem()` isn't taken for one.
  bool looksUpThroughHost(TSNode call) const
  {
    const TSNode receiver = receiverOf(call);

    if (ts_node_is_null(receiver) || kind(receiver) == Kind::This)
      return true;

    std::string lowered(lastName(text(receiver)));

    for (char& c : lowered)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return lowered.find("registry") != std::string::npos ||
           lowered.find("scene") != std::string::npos;
  }

  // ------------------------------------------------------------- services

  // service-update-method: a service's method named as a system's update or
  // hook, which nothing calls.
  void checkServiceMethod(TSNode name, std::string_view method)
  {
    if (ts_node_is_null(name) || !contains(LATE_METHODS, method))
      return;

    report(RuleId::ServiceUpdateMethod,
           name,
           "A Service has no update or hooks, so " + std::string(method) +
               "() is never called. Do the work in an event handler, or make "
               "it a System.");
  }

  // service-reaches-registry: a service's member, parameter (its methods',
  // its constructors', their lambdas') or local whose written type holds an
  // sfs::Registry or sfs::Scene (by pointer, reference or value, or wrapped:
  // `std::unique_ptr<sfs::Scene>`), or an sfs::QueryView handed in. An `auto`
  // local's type isn't written, so it isn't seen.
  void checkServiceReach(TSNode node)
  {
    const bool inService = functions.empty()
                               ? !classes.empty() && classes.back().service
                               : functions.back().service;

    if (!inService)
      return;

    const TSNode type = field(node, "type");

    if (ts_node_is_null(type))
      return;

    std::string_view held;
    TSNode at;
    lint::walkTree(
        type,
        [&](TSNode candidate)
        {
          if (lint::isType(candidate, "abstract_function_declarator"))
            return lint::Next::Skip;

          held = reachedBy(candidate);

          if (held.empty())
            return lint::Next::Descend;

          at = candidate;
          return lint::Next::Stop;
        });

    if (held.empty())
      return;

    report(RuleId::ServiceReachesRegistry,
           at,
           "A Service works on the entities its events hand it and never "
           "reaches " +
               std::string(held) +
               ". Do work over lists of entities in a System.");
  }

  // What a written type names that a service mustn't hold: "the registry",
  // "the scene" or "a list of entities" (an sfs::QueryView); empty for
  // anything else.
  std::string_view reachedBy(TSNode type) const
  {
    const Kind k = kind(type);

    if (k == Kind::QualifiedIdentifier || lint::isType(type, "type_identifier"))
    {
      const std::string_view spelled = text(type);

      if (isSfsName(spelled, "Registry"))
        return "the registry";

      if (isSfsName(spelled, "Scene"))
        return "the scene";
    }

    if (k == Kind::QualifiedIdentifier && text(field(type, "scope")) == "sfs")
    {
      const TSNode name = field(type, "name");

      if (kind(name) == Kind::TemplateType &&
          text(field(name, "name")) == "QueryView")
        return "a list of entities";
    }

    if (k == Kind::TemplateType && text(field(type, "name")) == "QueryView" &&
        kind(lint::parent(type)) != Kind::QualifiedIdentifier &&
        visible("QueryView"))
      return "a list of entities";

    return {};
  }

  // engine-internal: a name in sfs::detail in a project's code, written
  // `sfs::detail::...`, or `detail::...` where SFS's names are visible. The
  // SFS tree isn't a project, so it isn't checked.
  void checkEngineInternal(TSNode node)
  {
    if (!index->enabled() ||
        kind(lint::parent(node)) == Kind::QualifiedIdentifier ||
        !namesSfsDetail(text(node)))
      return;

    reportEngineInternal(node);
  }

  // engine-internal for a namespace alias's target (`namespace d =
  // sfs::detail;`), which isn't a qualified identifier.
  void checkNamespaceAlias(TSNode node)
  {
    const uint32_t count = ts_node_named_child_count(node);

    if (!index->enabled() || count == 0)
      return;

    const TSNode target = ts_node_named_child(node, count - 1);

    if (kind(target) != Kind::QualifiedIdentifier &&
        namesSfsDetail(text(target)))
      reportEngineInternal(target);
  }

  // Whether a spelled name is sfs::detail or in it (`sfs::detail::X`, with or
  // without a leading `::`), or `detail::X` where SFS's names are visible.
  bool namesSfsDetail(std::string_view spelled) const
  {
    if (spelled.starts_with("::"))
      spelled.remove_prefix(2);

    const auto under = [&](std::string_view prefix)
    {
      return spelled.starts_with(prefix) &&
             (spelled.size() == prefix.size() ||
              spelled[prefix.size()] == ':' || spelled[prefix.size()] == '<');
    };
    return under("sfs::detail") ||
           (spelled.starts_with("detail::") && (usingSfs || sfsDepth > 0));
  }

  void reportEngineInternal(TSNode node)
  {
    report(RuleId::EngineInternal,
           node,
           "`sfs::detail` is the engine's internals: it changes without "
           "notice, and some of it opens what the API keeps closed. Use the "
           "public API.");
  }

  void checkPerEntityMap(TSNode node)
  {
    TSNode type = field(node, "type");

    if (kind(type) == Kind::QualifiedIdentifier)
      type = field(type, "name");

    if (kind(type) != Kind::TemplateType ||
        !contains(ENTITY_MAPS, text(field(type, "name"))))
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
           "A " + std::string(classes.back().service ? "service" : "system") +
               " should not keep per-entity data in a map keyed by entity. "
               "Put the data on a component, in a hidden field.");
  }

  // SFS's EventConsumer, EventRequester, EventBroadcast or Responder in a
  // written type, in a wrapper too (`std::vector<sfs::Responder<R>>`), or an
  // alias of the file for one: its name, or empty. A function type's
  // parameters (`std::function<void(sfs::Responder<R>)>`) don't hold one.
  std::string_view eventHandleIn(TSNode written) const
  {
    std::string_view handle;
    lint::walkTree(written,
                   [&](TSNode type)
                   {
                     if (lint::isType(type, "abstract_function_declarator"))
                       return lint::Next::Skip;

                     if (lint::isType(type, "type_identifier"))
                     {
                       for (const auto& [alias, aliased] : handleAliases)
                         if (alias == text(type))
                         {
                           handle = aliased;
                           return lint::Next::Stop;
                         }

                       return lint::Next::Skip;
                     }

                     if (kind(type) != Kind::TemplateType)
                       return lint::Next::Descend;

                     const std::string_view name = text(field(type, "name"));

                     if (!contains(codegen::kEventHandleTypes, name))
                       return lint::Next::Descend;

                     const TSNode scope = lint::parent(type);
                     const bool sfs = kind(scope) == Kind::QualifiedIdentifier
                                          ? text(field(scope, "scope")) == "sfs"
                                          : visible(name);

                     if (!sfs)
                       return lint::Next::Descend;

                     handle = name;
                     return lint::Next::Stop;
                   });
    return handle;
  }

  // `using X = sfs::EventConsumer<E>;`, `typedef sfs::EventConsumer<E> X;`
  // and the like: remembered, so a member of type X holds a handle too.
  void readHandleAlias(TSNode node)
  {
    const std::string_view handle = eventHandleIn(field(node, "type"));
    const TSNode name =
        field(node, kind(node) == Kind::TypeDefinition ? "declarator" : "name");

    if (!handle.empty() && !ts_node_is_null(name))
      handleAliases.emplace_back(text(name), handle);
  }

  void checkReaderOnComponent(TSNode node)
  {
    if (kind(field(node, "declarator")) == Kind::FunctionDeclarator)
      return;

    const std::string_view handle = eventHandleIn(field(node, "type"));

    if (handle.empty())
      return;

    const std::string start =
        "The component " + std::string(classes.back().name) + " holds ";

    if (handle == "EventConsumer")
      report(RuleId::ReaderOnComponent,
             node,
             start + "an event consumer, which belongs to the system or "
                     "object that reads, not a component. Keep it as a "
                     "member of that system or object.");
    else if (handle == "EventRequester")
      report(RuleId::ReaderOnComponent,
             node,
             start + "an event requester, which belongs to the system or "
                     "object that asks, not a component. Keep it as a "
                     "member of that system or object.");
    else if (handle == "EventBroadcast")
      report(RuleId::ReaderOnComponent,
             node,
             start + "an event broadcast, which belongs to the system that "
                     "sends and the job it hands it to, not a component. "
                     "Keep it in that system or job.");
    else
      report(RuleId::ReaderOnComponent,
             node,
             start + "a responder, which belongs to the system or job that "
                     "responds to the request, not a component. Keep it in "
                     "that system's work list or in the job.");
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

    if (contains(STANDARD_ATTRIBUTES, plain))
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

    for (const std::string_view standard : STANDARD_ATTRIBUTES)
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
      {"type_definition", Kind::TypeDefinition},
      {"namespace_alias_definition", Kind::NamespaceAliasDefinition},
      {"preproc_include", Kind::PreprocInclude},
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
                 const FileReader* read,
                 const std::vector<codegen::EventModel>* events)
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

  if (events)
    responseTypesOf(*events, path, walk.responseTypes);

  walk.checkResponses();
  lint::runEntityRules(walk);
}

} // namespace sfs::lsp
