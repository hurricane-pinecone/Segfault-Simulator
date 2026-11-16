#pragma once

#include "lints.h"
#include "projectIndex.h"

#include <sfs/codegen/model.h>

#include <tree_sitter/api.h>

#include <cstdint>
#include <cstring>
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
 * The last name of a qualified spelling: "game::SpawnSystem" gives
 * "SpawnSystem".
 */
std::string_view lastName(std::string_view spelled);

/**
 * The top-level items of a comma-separated list (template arguments), each
 * trimmed: "A, sfs::Any<B, C>" gives "A" and "sfs::Any<B, C>".
 */
std::vector<std::string_view> splitTerms(std::string_view list);

/** The source range between two points. */
codegen::SourceRange between(TSPoint start, TSPoint end);

// Tree-sitter's node accessors, made safe on a null node (which they would
// dereference): no parent, children or siblings, an empty type, byte 0.

/** The node's parent, or null. */
inline TSNode parent(TSNode node)
{
  return ts_node_is_null(node) ? TSNode{} : ts_node_parent(node);
}

/** The node's first byte, or 0. */
inline uint32_t start(TSNode node)
{
  return ts_node_is_null(node) ? 0 : ts_node_start_byte(node);
}

/** The byte after the node, or 0. */
inline uint32_t end(TSNode node)
{
  return ts_node_is_null(node) ? 0 : ts_node_end_byte(node);
}

/** How many named children the node has. */
inline uint32_t namedCount(TSNode node)
{
  return ts_node_is_null(node) ? 0 : ts_node_named_child_count(node);
}

/** The node's named child `index`, or null past the last. */
inline TSNode namedChild(TSNode node, uint32_t index)
{
  return index < namedCount(node) ? ts_node_named_child(node, index) : TSNode{};
}

/** How many children, named or not, the node has. */
inline uint32_t childCountOf(TSNode node)
{
  return ts_node_is_null(node) ? 0 : ts_node_child_count(node);
}

/** The node's child `index`, named or not, or null past the last. */
inline TSNode nthChild(TSNode node, uint32_t index)
{
  return index < childCountOf(node) ? ts_node_child(node, index) : TSNode{};
}

/** The next named sibling, or null. */
inline TSNode nextNamed(TSNode node)
{
  return ts_node_is_null(node) ? TSNode{} : ts_node_next_named_sibling(node);
}

/** The previous named sibling, or null. */
inline TSNode prevNamed(TSNode node)
{
  return ts_node_is_null(node) ? TSNode{} : ts_node_prev_named_sibling(node);
}

/** The previous sibling, named or not (a token), or null. */
inline TSNode prevToken(TSNode node)
{
  return ts_node_is_null(node) ? TSNode{} : ts_node_prev_sibling(node);
}

/** The node's tree-sitter type name; empty for a null node. */
inline const char* typeName(TSNode node)
{
  return ts_node_is_null(node) ? "" : ts_node_type(node);
}

/** Whether two nodes are the same node, neither null. */
inline bool same(TSNode a, TSNode b)
{
  return !ts_node_is_null(a) && !ts_node_is_null(b) && ts_node_eq(a, b);
}

/** Whether `inner` lies within `outer`'s bytes. */
inline bool within(TSNode inner, TSNode outer)
{
  return !ts_node_is_null(outer) && start(outer) <= start(inner) &&
         end(inner) <= end(outer);
}

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

/** Whether `node` has the tree-sitter type `type`; false for a null node. */
inline bool isType(TSNode node, const char* type)
{
  return std::strcmp(typeName(node), type) == 0;
}

/**
 * The name a call calls and its template arguments: `f<T>()`, `x.f<T>()`,
 * `x->template f<T>()` or `x.f(...)`.
 * @param call a call expression
 * @param source the file's text
 * @param templateArguments set to the template argument list, or null
 * @return the name; empty for a qualified callee (`ns::f()`)
 */
std::string_view
calleeName(TSNode call, std::string_view source, TSNode& templateArguments);

/**
 * The name a declarator declares, through init, reference and pointer
 * declarators.
 * @param declarator the declarator
 * @param source the file's text
 * @return the name, or empty for another form
 */
std::string_view declaredName(TSNode declarator, std::string_view source);

/**
 * The class a node is written in: the class body around it, or the class an
 * out-of-line method around it belongs to (`ns::X::f` gives "X").
 * @param node the node
 * @param source the file's text
 * @return the class's unqualified name; empty outside classes
 */
std::string_view ownerOf(TSNode node, std::string_view source);

/**
 * The namespaces code at a node is in, outermost first (codegen's
 * lookupCandidates() takes them): those around it ("a::b" for a nested one,
 * "" for an unnamed one), then the qualifier of the out-of-line definition
 * around it (`void nav::NavSystem::update()` adds "nav" and "NavSystem").
 * @param node the node
 * @param source the file's text
 * @param out cleared, then gains each namespace
 */
void namespacesOf(TSNode node,
                  std::string_view source,
                  std::vector<std::string>& out);

/**
 * Where a local or parameter called `name` that `at` sees is declared: in a
 * block, a statement's init or condition, a range-for, or the parameters of
 * a function or lambda around `at`, before it. A class's members aren't
 * looked at.
 * @param at where the name is used
 * @param name the name
 * @param source the file's text
 * @return its declarator (an init declarator when it has a value), or null
 */
TSNode
localDeclarator(TSNode at, std::string_view name, std::string_view source);

/**
 * The declaration of a data member `name` in a class's body.
 * @param classNode a class or struct specifier
 * @param name the member's name
 * @param source the file's text
 * @return the field declaration, or null when the body has none
 */
TSNode
memberDeclaration(TSNode classNode,
                  std::string_view name,
                  std::string_view source);

/**
 * The type an expression makes: `T{...}`'s or `T(...)`'s, where T names a
 * type.
 * @param value the expression
 * @param source the file's text
 * @return the type as written, or empty for anything else
 */
std::string_view madeType(TSNode value, std::string_view source);

/**
 * The type of a local or parameter `name` that `at` sees, as its declaration
 * writes it (`const` and references aside); for `auto`, the type its value
 * makes (`auto done = R{}`).
 * @param at where the name is used
 * @param name the name
 * @param source the file's text
 * @return the type as written, or empty when it can't be read
 */
std::string_view
localTypeOf(TSNode at, std::string_view name, std::string_view source);

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
  /**
   * Whether it's a system: its base clause names SFS's System, or another
   * system class of the file or the project.
   */
  bool system;
  /**
   * Whether it's a component: marked [[sfs::component]] (and no event
   * attribute), or the baseless namespace-scope class the project index has
   * as a component.
   */
  bool component;
  /**
   * Whether it's a service: its base clause names SFS's Service, or another
   * service class of the file or the project.
   */
  bool service;
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

  /** Whether `name` is a Service class of the file or the project. */
  bool isServiceClass(std::string_view name) const;

  /** The file's class called `name`, or null. */
  const FileClass* fileClass(std::string_view name) const;

  /**
   * The class a node is written in (ownerOf(), unqualified). Empty outside
   * classes.
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
