#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The rules every diagnostic of sfs-codegen and sfs-lsp belongs to. A rule's
// name is what a diagnostic carries and what an sfs-ignore comment names.
namespace sfs::codegen
{

/**
 * How bad a rule's diagnostics are. Errors stop codegen and can't be ignored.
 */
enum class Severity : uint8_t
{
  Warning,
  Error,
};

/**
 * What reports a rule, which sets the diagnostic's LSP source: codegen
 * ("sfs-codegen"), an sfs-lsp lint ("sfs"), or the sfs-ignore parser both
 * tools share ("sfs").
 */
enum class RuleSource : uint8_t
{
  Codegen,
  Lint,
  Suppression,
};

/**
 * Every rule, in the table's order (Count last). EngineNamespace,
 * RenamedEngineType and StaleSavedName are reserved for the engine's rename
 * support (RuleInfo::reserved).
 */
enum class RuleId : uint8_t
{
  NoScriptType,
  DuplicateType,
  ScriptBase,
  ComponentBase,
  EngineNamespace,
  ComponentNotRegistered,
  FieldType,
  EntityField,
  UnknownSfsAttribute,
  AttributeTarget,
  AttributeArguments,
  AttributeFieldType,
  ShowIfTarget,
  SingleAny,
  AnyAlwaysMet,
  DuplicateTerm,
  ExcludedAnyMember,
  MatchesNothing,
  SecondAny,
  LateRegister,
  HookMutates,
  SystemReadsSystem,
  PerEntityMap,
  OwnEntity,
  UnknownAttribute,
  RenamedEngineType,
  StaleSavedName,
  HasThenGet,
  IsAlive,
  RowLookup,
  SetupInConstructor,
  ComponentPointer,
  FilterInLoop,
  UnusedRowEntity,
  RowComponentCopy,
  BadIgnore,
  Count,
};

/** A set of rules, e.g. those one sfs-ignore comment names. */
using RuleSet = std::bitset<static_cast<std::size_t>(RuleId::Count)>;

/**
 * One rule: what its diagnostics carry, what sfs-ignore names, and what the
 * docs list.
 */
struct RuleInfo
{
  /** The rule's enumerator. */
  RuleId id;
  /**
   * The stable id, kebab-case, e.g. "no-script-type". Never renamed or reused
   * once shipped.
   */
  std::string_view name;
  /** Warning or error. Every diagnostic of a rule has its rule's severity. */
  Severity severity;
  /** Codegen, a lint, or the sfs-ignore parser. */
  RuleSource source;
  /** Its diagnostics mark redundant code (the LSP Unnecessary tag). */
  bool unnecessary;
  /** Its diagnostics mark a deprecated name (the LSP Deprecated tag). */
  bool deprecated;
  /**
   * Nothing reports it yet: it's kept out of completion, the docs and
   * sfs-ignore until something does.
   */
  bool reserved;
  /** One sentence: the docs table, completion and hover. */
  std::string_view summary;
};

/** Every rule, in the order the docs list them (the RuleId order). */
std::span<const RuleInfo> rules();

/** The rule `id`. */
const RuleInfo& ruleInfo(RuleId id);

/** The rule called `name`, or null; null for a reserved rule. */
const RuleInfo* findRule(std::string_view name);

/**
 * The rule a misspelt `name` most likely meant (case-insensitive, at most 2
 * edits), or null.
 */
const RuleInfo* closestRule(std::string_view name);

/**
 * Whether an sfs-ignore comment may hide `rule`'s diagnostics: every warning
 * rule except bad-ignore.
 */
bool canIgnore(RuleId rule);

/** The tool a rule's diagnostics come from: "sfs-codegen", "sfs-lsp", or both.
 */
std::string_view toolName(RuleSource source);

/** "error" or "warning". */
std::string_view severityName(Severity severity);

/** The docs' rule table (docs/editor/sfs-lsp/index.md), as Markdown. */
std::string ruleDocsTable();

} // namespace sfs::codegen
