#pragma once

#include "lints.h"
#include "projectIndex.h"

#include <sfs/codegen/model.h>

#include <tree_sitter/api.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What sfs-lsp's lints share: text helpers, and one file's run (LintFile),
// which the walk (lints.cpp) fills and the entity API rules (entityLints.cpp)
// read once it's done.
namespace sfs::lsp::lint
{

/** Whether `c` is whitespace. */
bool isSpace(char c);

/**
 * Whether `text`, whitespace aside, begins with the parts one after another;
 * with `whole`, whether it's exactly them.
 */
bool spelledAs(std::string_view text,
               std::initializer_list<std::string_view> parts,
               bool whole = true);

/** Whether two spellings are the same, whitespace aside. */
bool sameSpelling(std::string_view a, std::string_view b);

/** `text` with each run of whitespace made one space, and none at the ends. */
std::string tidy(std::string_view text);

/** `text` without whitespace at either end. */
std::string_view trimmed(std::string_view text);

/**
 * The last name of a qualified spelling: "game::FieldSystem" gives
 * "FieldSystem".
 */
std::string_view lastName(std::string_view spelled);

/**
 * The top-level items of a comma-separated list (template arguments), each
 * trimmed: "A, sfs::Any<B, C>" gives "A" and "sfs::Any<B, C>".
 */
std::vector<std::string_view> splitTerms(std::string_view list);

/** The source range between two points. */
codegen::SourceRange between(TSPoint start, TSPoint end);

/** What a tree walk does after visiting a node. */
enum class Next : uint8_t
{
  Descend,
  Skip,
  Stop,
};

/**
 * Visits `root` and its descendants in order, without recursion (deeply nested
 * code can't overflow the stack).
 *
 * @param root where the walk starts; a null node is an empty walk
 * @param visit called on each node; says whether to go into its children,
 *   skip them, or stop the walk
 * @param leave called on each node visited once its children are done (not
 *   after a stop)
 */
template <typename Visit, typename Leave>
void walkTree(TSNode root, Visit&& visit, Leave&& leave)
{
  if (ts_node_is_null(root))
    return;
  TSTreeCursor cursor = ts_tree_cursor_new(root);
  bool more = true;
  while (more)
  {
    const TSNode node = ts_tree_cursor_current_node(&cursor);
    const Next next = visit(node);
    if (next == Next::Stop)
      break;
    if (next == Next::Descend && ts_tree_cursor_goto_first_child(&cursor))
      continue;
    leave(node);
    // On to the next node: a sibling, or an ancestor's, leaving the ancestors
    // passed on the way.
    while (more && !ts_tree_cursor_goto_next_sibling(&cursor))
    {
      more = ts_tree_cursor_goto_parent(&cursor);
      if (more)
        leave(ts_tree_cursor_current_node(&cursor));
    }
  }
  ts_tree_cursor_delete(&cursor);
}

/** walkTree() with nothing to do on leaving a node. */
template <typename Visit>
void walkTree(TSNode root, Visit&& visit)
{
  walkTree(root, std::forward<Visit>(visit), [](TSNode) {});
}

/** An SFS wrapper a query term may be written in. */
enum class Wrapper : uint8_t
{
  None,
  Any,
  Excluding,
  Query,
};

/** A class or struct with a body, declared in the file. */
struct FileClass
{
  /** Its name; empty for an unnamed one. */
  std::string_view name;
  /** Its class_specifier (or struct_specifier) node. */
  TSNode node;
  /** Whether its base clause names SFS's System. */
  bool system;
};

/** A registerComponent<T>() in a system's constructor or create(). */
struct Registration
{
  /** The system's name. */
  std::string_view system;
  /** T's node. */
  TSNode argument;
  /** The statement it's the whole of, or null. */
  TSNode statement;
};

/**
 * One run of the lints over a file: the file, what the walk learnt of it, and
 * the helpers every lint reads it with. Names are views into the file's text,
 * which outlives the run.
 */
struct LintFile
{
  using Kind = Linter::Kind;

  /** The linter, for its node kinds. */
  const Linter* linter = nullptr;
  /** The file's text. */
  std::string_view source;
  /** The file, for the diagnostics. */
  const std::filesystem::path* path = nullptr;
  /** The project's types and query aliases. */
  const ProjectIndex* index = nullptr;
  /** Where the diagnostics go. */
  std::vector<codegen::Diagnostic>* out = nullptr;
  /** Gives other files' text (a class's header). */
  const FileReader* read = nullptr;

  /** `using namespace sfs;` seen. */
  bool usingSfs = false;
  /** Namespace sfs scopes the walk is in. */
  int sfsDepth = 0;
  /** Names brought in by `using sfs::X;`. */
  std::vector<std::string_view> usingNames;
  /** Every class with a body, in the order written. */
  std::vector<FileClass> fileClasses;
  /** The registerComponent calls of the file's systems. */
  std::vector<Registration> registrations;
  /** The file's own query aliases. */
  std::vector<QueryAlias> aliases;

  /** hasComponent<T>() calls, for has-then-get. */
  std::vector<TSNode> hasCalls;
  /** isAlive() calls, for is-alive. */
  std::vector<TSNode> aliveCalls;
  /** getComponent<T>() calls, for row-lookup and component-pointer. */
  std::vector<TSNode> lookups;
  /** Range-for loops, for the row rules (row-lookup, filter-in-loop, ...). */
  std::vector<TSNode> loops;
  /** Constructor definitions (in class or out of line). */
  std::vector<TSNode> constructors;

  /** Forgets the last file's state, keeping the containers' capacity. */
  void resetFile();

  /** The kind of `node`. */
  Kind kind(TSNode node) const { return linter->kindOf(node); }

  /** The text `node` covers. */
  std::string_view text(TSNode node) const;

  /** A node's child by field name; null for a null node. */
  static TSNode field(TSNode node, std::string_view name);

  /** Adds a diagnostic of `rule` on `node`. */
  void report(codegen::RuleId rule,
              TSNode node,
              std::string message,
              std::string fixTitle = {},
              std::vector<codegen::TextEdit> fix = {});

  /** Whether an unqualified SFS name means SFS's where the walk is. */
  bool visible(std::string_view name) const;

  /**
   * Whether a spelling names SFS's `name` where the walk is: qualified, or
   * unqualified where SFS's `visibleAs` (by default `name`) is visible.
   */
  bool isSfsName(std::string_view spelled,
                 std::string_view name,
                 std::string_view visibleAs = {}) const;

  /**
   * isSfsName() at a given node once the walk is done: inside namespace sfs,
   * or after the file's using declarations.
   */
  bool
  isSfsNameAt(TSNode at, std::string_view spelled, std::string_view name) const;

  /**
   * The name a declarator declares (through init, reference and pointer
   * declarators), or empty.
   */
  std::string_view declaredName(TSNode declarator) const;

  /** The name of a function's or lambda's first parameter, or empty. */
  std::string_view firstParameter(TSNode parameters) const;

  /**
   * The name a call calls (`f`, `x.f`, `x->f<T>`), and its template
   * arguments.
   */
  std::string_view calleeName(TSNode call, TSNode& templateArguments) const;

  /**
   * The object a call is made on (`x` of `x.f()`), or null for a plain call.
   */
  TSNode receiverOf(TSNode call) const;

  /** Whether `name` is a System class of the file or the project. */
  bool isSystemClass(std::string_view name) const;

  /** The file's class called `name`, or null. */
  const FileClass* fileClass(std::string_view name) const;

  /**
   * The class a node is written in: the class body around it, or the class an
   * out-of-line method around it belongs to. Empty outside classes.
   */
  std::string_view ownerAt(TSNode node) const;

  /**
   * The query alias `name` as seen from class `owner`: the file's own, then
   * the project's.
   */
  const QueryAlias* alias(std::string_view owner, std::string_view name) const;

  /**
   * The SFS wrapper a written term is (`sfs::Any<...>`, `Excluding<...>` where
   * SFS's is visible at `at`, `sfs::Query<...>`), and its arguments' text.
   */
  Wrapper wrapperOfText(TSNode at,
                        std::string_view spelled,
                        std::string_view& inner) const;
};

/**
 * The entity API rules (entityLints.cpp): has-then-get, is-alive, row-lookup,
 * filter-in-loop, unused-row-entity, row-component-copy, setup-in-constructor
 * and component-pointer, run over the sites the walk found once it's done.
 * Their diagnostics go to file.out.
 *
 * @param file the run, with the walk done and the system registrations read
 */
void runEntityRules(LintFile& file);

} // namespace sfs::lsp::lint
