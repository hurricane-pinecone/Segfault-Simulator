#include "sfs/codegen/rules.h"

#include "sfs/codegen/attributes.h"
#include "sfs/codegen/model.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace sfs::codegen
{

namespace
{

using enum RuleId;
constexpr Severity W = Severity::Warning;
constexpr Severity E = Severity::Error;
constexpr RuleSource C = RuleSource::Codegen;
constexpr RuleSource L = RuleSource::Lint;

// A new rule goes at the end of its tool's rows (a house rule at the very end,
// after bad-ignore), with its RuleId enumerator in the same place. Names are
// never changed once shipped: sfs-ignore comments name them.
constexpr std::array<RuleInfo, static_cast<std::size_t>(Count)> RULES = {{
    {NoScriptType,
     "no-script-type",
     W,
     C,
     false,
     false,
     false,
     "A header codegen reads should declare a class or struct named "
     "after the file. Name the script type after the file, or mark a "
     "helper header with `sfs-ignore-file(no-script-type)`."},
    {DuplicateType,
     "duplicate-type",
     E,
     C,
     false,
     false,
     false,
     "Two types should not have one registered name. Give each type a "
     "name of its own."},
    {ScriptBase,
     "script-base",
     E,
     C,
     false,
     false,
     false,
     "A script type has at most one base, and it must be one codegen "
     "knows. Derive from one engine base or one reflected type."},
    {ComponentBase,
     "component-base",
     E,
     C,
     false,
     false,
     false,
     "An `[[sfs::component]]` type should not have a base. Give it the "
     "fields itself, or put shared data in a component of its own."},
    {EngineNamespace,
     "engine-namespace",
     E,
     C,
     false,
     false,
     true,
     "A game type should not be declared in namespace `sfs`, which the "
     "engine reserves. Move it to the game's own namespace."},
    {ComponentNotRegistered,
     "component-not-registered",
     W,
     C,
     false,
     false,
     false,
     "An unnamed, nested or template type can't be registered as a "
     "component. Declare the component as a plain named type at "
     "namespace scope."},
    {FieldType,
     "field-type",
     W,
     C,
     false,
     false,
     false,
     "The editor can't edit pointer, reference, array or unknown-type "
     "fields, so they're skipped. Store a value the editor can edit "
     "instead."},
    {EntityField,
     "entity-field",
     W,
     C,
     false,
     false,
     false,
     "Entity reference fields belong on components, so they're skipped "
     "on other types. Move the field to a component."},
    {UnknownSfsAttribute,
     "unknown-sfs-attribute",
     W,
     C,
     false,
     false,
     false,
     "An `[[sfs::...]]` attribute should be one the attribute table "
     "knows. Fix the name, or remove the attribute."},
    {AttributeTarget,
     "attribute-target",
     W,
     C,
     false,
     false,
     false,
     "A field attribute should not be on a type, nor a type attribute on "
     "a field. Move the attribute to where it belongs."},
    {AttributeArguments,
     "attribute-arguments",
     W,
     C,
     false,
     false,
     false,
     "An attribute's arguments should have the shape it takes. Write "
     "them as the attribute table shows."},
    {AttributeFieldType,
     "attribute-field-type",
     W,
     C,
     false,
     false,
     false,
     "An attribute should only be on the field types it applies to. "
     "Remove it, or change the field's type."},
    {ShowIfTarget,
     "show-if-target",
     W,
     C,
     false,
     false,
     false,
     "`[[sfs::showIf]]` should name a reflected `bool` field of its "
     "type. Name one of the type's bool fields."},
    {EventAndComponent,
     "event-and-component",
     W,
     C,
     false,
     false,
     false,
     "A type should not be marked both as an event (`[[sfs::event_news]]` "
     "and the like) and `[[sfs::component]]`, since an event is what one "
     "system tells another and a component is state; it's taken as an "
     "event. Remove one of the two marks."},
    {EventDeclaration,
     "event-declaration",
     E,
     C,
     false,
     false,
     false,
     "An event type must be declared so the compiler can check it: one "
     "event attribute, on a named struct at namespace scope outside inline "
     "namespaces; a request naming its success response first, then any "
     "`progress =` and `failure =` ones, each an `[[sfs::event_response]]` "
     "type in one role; "
     "news deriving only from news of its own kind, each base once, and "
     "requests and responses deriving from nothing; and no name declared "
     "twice differently. An attribute on an enum can't be read, so it isn't "
     "checked."},
    {SceneDataDeclaration,
     "scene-data-declaration",
     E,
     C,
     false,
     false,
     false,
     "`[[sfs::scene_data]]` marks data the scene holds one of, so it goes on a "
     "named plain struct or class at namespace scope with no base, never on "
     "a component, an event, a system, a service, a game object or a "
     "scriptable object."},
    {SingleAny,
     "single-any",
     W,
     L,
     true,
     false,
     false,
     "An `sfs::Any` group of one component is just that component. Write "
     "the component instead."},
    {AnyAlwaysMet,
     "any-always-met",
     W,
     L,
     true,
     false,
     false,
     "An `sfs::Any` group that shares a component with the required ones "
     "is always met. Remove the group."},
    {DuplicateTerm,
     "duplicate-term",
     W,
     L,
     true,
     false,
     false,
     "A term list should not name a component twice in one role. Remove "
     "the repeat."},
    {ExcludedAnyMember,
     "excluded-any-member",
     W,
     L,
     true,
     false,
     false,
     "A member of an `sfs::Any` group should not also be excluded, since "
     "it then never matches. Remove it from the group."},
    {MatchesNothing,
     "matches-nothing",
     W,
     L,
     false,
     false,
     false,
     "A term list should not require and exclude one component, or "
     "exclude every member of its `sfs::Any` group, since it then "
     "matches nothing. Keep each component in one role."},
    {SecondAny,
     "second-any",
     W,
     L,
     false,
     false,
     false,
     "A system takes one `sfs::Any` group, and a second one is ignored "
     "at run time. List every alternative in one group."},
    {WithInRegister,
     "with-in-register",
     W,
     L,
     true,
     false,
     false,
     "An `sfs::With` group written in `registerComponent<...>()` or "
     "`observe<...>()` is just its components, since neither binds a row. "
     "Register or observe the components themselves."},
    {LateRegister,
     "late-register",
     W,
     L,
     false,
     false,
     false,
     "`registerComponent()` should not run once the system is in a "
     "registry, where it's ignored. Register components in `create()`."},
    {HookMutates,
     "hook-mutates",
     W,
     L,
     false,
     false,
     false,
     "`onEntityAdded()`, `onEntityRemoved()` and `observe` callbacks "
     "should change only their own entity's components. Change other "
     "entities, and create or destroy entities and systems, in "
     "`update()`."},
    {SystemReadsSystem,
     "system-reads-system",
     W,
     L,
     false,
     false,
     true,
     "Systems should not read other systems. Read the state another "
     "system writes on components, and send what one system tells another "
     "as an event."},
    {PerEntityMap,
     "per-entity-map",
     W,
     L,
     false,
     false,
     false,
     "A system or service should not keep per-entity data in a map or set "
     "keyed by entity. Put the data on a component, in a hidden field."},
    {OwnEntity,
     "own-entity",
     W,
     L,
     false,
     false,
     false,
     "A component should not store the entity it's on. Drop the field, "
     "and use the entity the system already has."},
    {UnknownAttribute,
     "unknown-attribute",
     W,
     L,
     false,
     false,
     false,
     "An attribute should be standard C++, an `sfs::` one, or in a "
     "vendor's namespace. Fix the name or the namespace."},
    {RenamedEngineType,
     "renamed-engine-type",
     W,
     L,
     false,
     true,
     true,
     "An engine type or header should not be used by a name the engine "
     "has renamed. Use its new name."},
    {StaleSavedName,
     "stale-saved-name",
     W,
     L,
     false,
     false,
     true,
     "A saved scene, prefab or asset should not name a type by a name it "
     "no longer has. Save it again with the new name."},
    {HasThenGet,
     "has-then-get",
     W,
     L,
     true,
     false,
     false,
     "A component should not be looked up twice, with "
     "`hasComponent<T>()` and then `getComponent<T>()`. Use "
     "`getComponent<T>()` alone, which gives null when T is missing."},
    {IsAlive,
     "is-alive",
     W,
     L,
     false,
     true,
     false,
     "`Entity::isAlive()` is gone, because a handle is now valid only "
     "while its entity lives. Use `isValid()`, or test the handle with "
     "`if (entity)`."},
    {RowLookup,
     "row-lookup",
     W,
     L,
     true,
     false,
     false,
     "A query loop should not look up a component its row already has. "
     "Unpack the row with `for (auto [e, ...] : ...)`."},
    {SetupInConstructor,
     "setup-in-constructor",
     W,
     L,
     false,
     false,
     false,
     "A system should not set itself up in its constructor. Register "
     "components and observers, and set the time step, in `create()`."},
    {ComponentPointer,
     "component-pointer",
     W,
     L,
     false,
     true,
     false,
     "`getComponent<T>()` gives a pointer, which is null when T is "
     "missing, so it should not be used as a reference. Use `->`, check "
     "it for null, and pass what it points to where `T&` is wanted."},
    {FilterInLoop,
     "filter-in-loop",
     W,
     L,
     false,
     false,
     false,
     "A loop should not go through a query, or every entity, only to skip "
     "the entities that lack a component. Go through a query that lists "
     "the component, so the loop only visits those that have it."},
    {UnusedRowEntity,
     "unused-row-entity",
     W,
     L,
     true,
     false,
     false,
     "A query loop should not unpack an entity it never uses. Loop over "
     "the components alone, with `T& t` for one or `.components()` for "
     "several."},
    {UnusedRowComponent,
     "unused-row-component",
     W,
     L,
     true,
     false,
     false,
     "A query loop should not bind a component it never uses. Require it "
     "with `sfs::With<T>`, which lists the same entities, and drop the "
     "binding."},
    {RowComponentCopy,
     "row-component-copy",
     W,
     L,
     false,
     false,
     false,
     "A query loop should not copy a row's component only to read it. Read "
     "it in place with `const T&`, or, for a copy it changes, keep it as "
     "scratch or use `T&`."},
    {MutableRead,
     "mutable-read",
     W,
     L,
     false,
     false,
     false,
     "Code that only reads a component or scene data should take it as "
     "const: `getComponent<const T>()`, `sceneData<const T>()`, a query's "
     "`const T` term. Then taking it mutably says the code changes it."},
    {EventHandleOutsideCreate,
     "event-handle-outside-create",
     W,
     L,
     false,
     false,
     false,
     "An event consumer or requester should be made in `create()` (an "
     "object's in `onCreate`) and kept as a member. Made anywhere else it "
     "receives nothing, or has no owner to run its handlers."},
    {ReaderOnComponent,
     "reader-on-component",
     W,
     L,
     false,
     false,
     false,
     "A component should not hold an `sfs::EventConsumer`, "
     "`sfs::EventRequester`, `sfs::EventBroadcast` or `sfs::Responder`, "
     "which belong to whoever reads, asks, sends or responds. Keep it in "
     "that system, object or job."},
    {ResponseSentTwice,
     "response-sent-twice",
     W,
     L,
     false,
     false,
     false,
     "A response type should be sent by one system or service, the one that "
     "consumes its request. Give each one's responses a type of their own."},
    {ServiceReachesRegistry,
     "service-reaches-registry",
     W,
     L,
     false,
     false,
     false,
     "A Service works on the entities its events hand it and never reaches "
     "the registry, the scene or a list of entities. Do work over lists of "
     "entities in a System."},
    {ServiceUpdateMethod,
     "service-update-method",
     W,
     L,
     false,
     false,
     false,
     "A Service has no update or hooks, so this is never called. Do the work "
     "in an event handler, or make it a System."},
    {EngineInternal,
     "engine-internal",
     W,
     L,
     false,
     false,
     false,
     "`sfs::detail` is the engine's internals: it changes without notice, "
     "and some of it opens what the API keeps closed. Use the public API."},
    {SystemLookup,
     "system-lookup",
     W,
     L,
     false,
     false,
     false,
     "Game code should not look systems up. Use the engine's facilities "
     "(`physics()`, `navigation()`, `graphics()`, ...) for engine features; "
     "read game state from components and scene data, and send what one part "
     "tells another as an event."},
    {KeepsFacility,
     "keeps-facility",
     W,
     L,
     false,
     false,
     false,
     "A facility is valid for the step that got it: get it where you use it, "
     "and hand workers its snapshots."},
    {BadIgnore,
     "bad-ignore",
     W,
     RuleSource::Suppression,
     false,
     false,
     false,
     "An `sfs-ignore` comment should name known warning rules in the "
     "right form. Fix the comment."},
    {ConstexprName,
     "constexpr-name",
     W,
     L,
     false,
     false,
     false,
     "A `constexpr` variable should be named in UPPER_SNAKE_CASE. Rename it "
     "(`kMaxItems` becomes `MAX_ITEMS`), and make a value someone may want to "
     "tune authored data instead.",
     true},
    {NestedTernary,
     "nested-ternary",
     W,
     L,
     false,
     false,
     false,
     "A ternary should not be nested inside another, though two kept apart are "
     "fine. Work the inner one out into its own named variable first, and use "
     "that in the outer ternary.",
     true},
    {ElseAfterReturn,
     "else-after-return",
     W,
     L,
     false,
     false,
     false,
     "An `else` should not follow a branch that ends in `return`, `continue`, "
     "`break` or `throw`. Drop the `else` and write its code after the `if`, "
     "so the main path isn't nested.",
     true},
}};

constexpr bool inOrder()
{
  for (std::size_t i = 0; i < RULES.size(); ++i)
    if (static_cast<std::size_t>(RULES[i].id) != i)
      return false;

  return true;
}
static_assert(inOrder(), "kRules must list every RuleId in order");

} // namespace

std::span<const RuleInfo> rules() { return RULES; }

const RuleInfo& ruleInfo(RuleId id)
{
  return RULES[static_cast<std::size_t>(id)];
}

const RuleInfo* findRule(std::string_view name)
{
  for (const RuleInfo& rule : RULES)
    if (rule.name == name && !rule.reserved)
      return &rule;

  return nullptr;
}

const RuleInfo* closestRule(std::string_view name)
{
  const RuleInfo* best = nullptr;
  std::size_t bestDistance = 3;

  for (const RuleInfo& rule : RULES)
  {
    if (rule.reserved)
      continue;

    const std::size_t distance = editDistance(name, rule.name);

    if (distance < bestDistance)
    {
      best = &rule;
      bestDistance = distance;
    }
  }

  return best;
}

bool canIgnore(RuleId rule)
{
  return rule != BadIgnore && ruleInfo(rule).severity == Severity::Warning;
}

std::string_view toolName(RuleSource source)
{
  switch (source)
  {
  case RuleSource::Codegen:
    return "sfs-codegen";
  case RuleSource::Lint:
    return "sfs-lsp";
  case RuleSource::Suppression:
    return "sfs-codegen, sfs-lsp";
  }

  return "sfs-lsp";
}

std::string_view severityName(Severity severity)
{
  return severity == Severity::Error ? "error" : "warning";
}

std::string formatDiagnostic(const Diagnostic& diagnostic, bool column)
{
  const RuleInfo& rule = ruleInfo(diagnostic.rule);
  std::string line = diagnostic.file.string() + ':' +
                     std::to_string(diagnostic.range.startRow + 1) + ':';

  if (column)
    line += std::to_string(diagnostic.range.startColumn + 1) + ':';

  line += ' ';
  line += severityName(rule.severity);
  line += ": " + diagnostic.message + " [";
  line += rule.name;
  line += ']';
  return line;
}

std::string ruleDocsTable()
{
  std::string out = "| Rule | Tool | Severity | What it says |\n"
                    "| --- | --- | --- | --- |\n";

  for (const RuleInfo& rule : RULES)
  {
    if (rule.reserved || rule.house)
      continue;

    out += "| `";
    out += rule.name;
    out += "` | ";
    out += toolName(rule.source);
    out += " | ";
    out += severityName(rule.severity);
    out += " | ";
    out += rule.summary;
    out += " |\n";
  }

  return out;
}

std::string houseRuleDocsTable()
{
  std::string out = "| Rule | What it says |\n"
                    "| --- | --- |\n";

  for (const RuleInfo& rule : RULES)
  {
    if (rule.reserved || !rule.house)
      continue;

    out += "| `";
    out += rule.name;
    out += "` | ";
    out += rule.summary;
    out += " |\n";
  }

  return out;
}

} // namespace sfs::codegen
