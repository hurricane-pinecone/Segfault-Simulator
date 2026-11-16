# sfs-lsp internals

How sfs-lsp's checks match code, rule by rule, where a game developer
doesn't need the detail. What the checks are for and how to use them:
[Code checks: sfs-lsp](../../../../docs/editor/sfs-lsp/index.md).

The lints read one file's syntax tree (tree-sitter), so they see names as
written, not as the compiler resolves them. The project index
(`projectIndex.cpp`) reads every `.h` and `.cpp` under a project's `scripts/`
folder: the types codegen registers (from the headers), and the responses
each class sends and the response types (from both). It's built in slices
while the server is idle, and an open buffer's text replaces its file's
entries when it's opened and saved (an unsaved change isn't indexed). A root
without `project.sfsproj` has no index.

## Event lints

### event-declaration

Codegen's error (`parser.cpp`, `generator.cpp`), shown live for a header the
editor has open. sfs-lsp runs `generateEventTraits()` on the header's own
events, so a request listing a type the header declares is resolved; one
declared elsewhere is left to the build, which scans a module's headers
together, and a listed type no scanned header declares is left to the
compiler. The cases are listed in [events
internals](../../core/events/index.md#declarations-and-the-traits-header).
An event attribute on a field is reported only where codegen reads the
fields: the file's script type and `[[sfs::component]]` types. tree-sitter's
C++ grammar misparses an attribute on an enum (`enum [[a]] E` becomes an
ERROR and a statement, `enum class [[a]] E` an enum without a name), so
enums aren't checked. A cycle of event bases (`A : B` with `B : A`, or
`Self : Self`) is reported on each event in it, and copies of a base aren't
counted in a cycle.

### event-handle-outside-create

The calls checked:
- in a system: `consumer<E>()` and `requester<R>()` with no receiver, on
  `this` (`this->template consumer<E>()` too) or as
  `sfs::EventVerbs::consumer<E>()`; and the same calls on an Events, which
  are always reported in a system (`Events::consumer()` gives a system's
  handle no owner);
- in a game object: `consumer<E>()` and `requester<R>()` on an Events.

An Events receiver is an `events()` call (`scene().events()`,
`registry->events()`), a local or parameter declared as `Events`
(`sfs::Events& queue`) or initialised from one (`auto& e = scene.events()`),
or another name ending in `events`, whatever case (`m_events`).

A class is a system when it derives from `sfs::System` (or `System` where
SFS's names are visible), from a system class of the file, or from one the
index knows; the index follows a registered type's base through `Derived`
entries. Game objects likewise, from `sfs::GameObject`. Other classes (a
`ui::Layer`'s widgets, jobs) and free functions aren't checked.

Code that runs once the class is set up, where a handle is reported:
- systems: `update`, `lateUpdate`, `render`, `onEntityAdded`,
  `onEntityRemoved`;
- game objects: `onUpdate`, `onDestroy`, `onProcessInput` and the collision
  and trigger callbacks (2D too);
- both: an `observe` callback, a lambda given to a handler setter
  (`onEvent`, `onEvents`, `onLatest`, `onLatestEvents`, `onResponse`,
  `onResponses`, `onProgress`, `onProgresses`, `onFailure`, `onFailures`),
  the constructor (its initialiser list included), the
  destructor, and a default member initialiser.

Any other method counts as set-up (`create()`, `onCreate()` or a helper they
call), and there the handle must be kept. The value is followed up from the
call through parentheses, a `?:` branch, braces (`= {...}`), a builder chain
(`requester<R>().onResponse(f).ignoreFailure()`), `std::move()` and a handle
type's constructor (`sfs::EventConsumer<E>(...)`). It's kept when it's then:
- assigned to a data member: `m_x`, `this->x`, a member the file's class
  declares (`lint::memberDeclaration()`), a part of one (`m_x.y`, `m_x[i]`,
  `*m_p`), or a local reference bound to one of those; a name a local or
  parameter in scope declares is never a member (`lint::localDeclarator()`,
  which looks only at the enclosing blocks, statements and parameters), and,
  out of line for a class the file doesn't declare, any other name is taken
  as one;
- added to a member container (`push_back`, `emplace_back`, `insert`,
  `emplace`, `try_emplace`, `push`), braces in the argument too;
- returned, or passed to another function (which may keep it);
- held by a local (not a condition's declaration) that a later use in its
  block keeps in one of these ways, two locals deep, or by a `static` local.

A data member's element by reference counts as the member: `m_map.at(k)`,
`m_list.back()` (any call on a member), `std::get<I>(m_tuple)`, and a `?:`
whose branches are both members. The walk up a class's bases looks at each
class once, so a cycle of base classes (code being typed may have one,
branching too) ends it.

Missed by design, since the lint reads one method at a time: a handle made
in a helper that both set-up and later code call (the helper counts as
set-up), in a lambda stored and called later, in a handler held in a
variable before it's given, or in a job's lambda.

### reader-on-component

A data member of a component (marked `[[sfs::component]]` and no event
attribute, or the baseless namespace-scope class its project header
registers) whose type holds a handle: `sfs::EventConsumer`,
`sfs::EventRequester`, `sfs::EventBroadcast` or `sfs::Responder`, qualified
or where SFS's names are visible, at any depth of template arguments
(`std::optional`, `std::unique_ptr`, `std::array`), or an alias the file
declares for such a type. A function type's parameters
(`std::function<void(sfs::Responder<R>)>`) aren't a held handle. Pointers to
a handle are reported like the handle. Codegen, for its part, skips a script
type's handle members without a `field-type` warning (`holdsEventHandle()`):
qualified, unqualified inside namespace sfs or after the header's
`using namespace sfs;`, unqualified after `using sfs::X;` for that X, in a
container, or through an alias (`using` or `typedef`) the header declares.
An alias declared in another header isn't seen, so its members still get
the warning; an unqualified handle name with none of these is taken as
someone else's type and warned about.

### response-sent-twice

A send is a responder's `x.respond(arg)`, `x.progress(arg)` or
`x.fail(arg)`, or a request consumer's by entity, `x.respond(e, arg)` and the
like (the last argument), whose type can be read: the template argument
of `x.respond<R>(...)`, else the argument's `R{...}`, `R(...)`,
`std::move(v)` or `v` for a local or parameter `v` declared with a type (for
`auto`, the type its value makes) or
a data member declared in the class body around the call. Spellings are
normalised as codegen normalises types (`codegen::normaliseType()`).

Response types come from codegen's event models (`responseTypesOf()`): a
struct marked `[[sfs::event_response]]` declares its qualified name; each
type an `[[sfs::event_request(...)]]` names in a role (success, `progress =`,
`failure =`) gives every name its spelling may mean from the request
(`lookUpName()`), as listed only.

A send's spelling is resolved from where it's written (`resolveResponse()`),
through `codegen::lookUpName()`, which codegen also uses for a request's
responses and an event's bases:
- the namespaces it's in are those around the call, a nested one's
  (`namespace a::b`) parts apart, then an out-of-line definition's qualifier
  (`void nav::NavSystem::update()` adds `nav` and `NavSystem`);
- a using name replaces what it names, looked up from where the using name
  is written, up to eight deep: a using-declaration
  (`using game::nav::Found;`) or type alias (`using NavFound = ...;`,
  `typedef`) for the whole spelling when the send sees it, or, at namespace
  scope, for a spelling that reaches it through its namespace
  (`namespace z { using a::Found; }` then `z::Found`); a namespace alias
  (`namespace g = game;`) for the spelling's first part. A using name is seen
  when it's written before the send, in a namespace the send is in, and,
  inside a block (a function body), only to the block's end. The using names
  are read by `codegen::readUsingName()`;
- the names it may mean (`codegen::lookupCandidates()`, innermost first),
  then those through each using-directive it sees (`using namespace nav;`);
- the first of them a script declares wins; failing that, the first a
  request lists (an engine type, which is checked once a project request
  lists it).

Two sends match when they resolve to the same name. A using name in an
included header isn't seen, and a using-directive's namespace is tried after
the names the send's own namespaces give, where C++ would call two such names
ambiguous.

The index keeps each script file's data apart (`ScriptFile`), so reading a
file again replaces only its own; the lookups above are rebuilt from the
files once after a change.

A send's system is the class it's written in, when that's a system;
otherwise (a job, a helper struct, a free function) the one system of its
file, found among the file's system classes and the owners of its sends (for
another file, the index's systems declared in that header and the owners of
its sends). With none or several, the send is nobody's and isn't compared.
A send is reported when another system, of the file or another file of the
index, sends the same type. The index resolves each of its sends' type and
system once after a change (`ProjectIndex::buildLookup()`) and keeps the
senders by type, so a file's check costs a lookup per send, and the whole
project's about as much as reading it. An engine response type no project
request lists isn't known, so its sends aren't checked.

## House lints

### The .sfs-lint lookup

`analyse()` (`analysis.cpp`) asks `lintConfigFor()` (`lintConfig.cpp`) for
the file's settings, so the server, `--check` and `--fix` all get them. It
looks for `.sfs-lint` in the file's folder and each folder above it
(`findLintConfig()`: a stat per folder, every analysis, so a file added or
removed counts at once). Each config file's settings are cached by path and
read again when its modification time or size changes; its warnings go to
stderr each time it's read, where the server's log and the CLI's errors go. A
relative path is looked up only through the folders it names (the server and
the CLI always pass absolute ones), so the tests' made-up paths (`/t/f.cpp`)
find none. With `include: house`, `runHouseLints()` (`houseLints.cpp`) runs
after the other lints; the file's `disable` list joins the user's disabled
rules before they're filtered out. Rules with `RuleInfo::house` come last in
the rule table, and `houseRuleDocsTable()` writes the docs' house table (a
test keeps the two alike).

The house lints are one walk of the tree (ERROR subtrees skipped) that
dispatches on node kinds by symbol. constexpr-name's checks wait until the
walk is done, since they need every constant of the file.

### constexpr-name

A `declaration` or `field_declaration` with a `constexpr` type qualifier;
each declarator, through init, pointer, reference and array declarators, to a
plain name (functions and qualified out-of-line definitions aren't
variables). It's reported when the name has a lower-case letter.

Its scope is the nearest of: a class body (class), a namespace body or the
file (namespace; one under a brace-less `extern "C"` is `extern`: no fix),
anything else (a block, a `case`, a `for`, `if` or `while` header: block).
The fix is offered for a block's constant, and for a namespace constant in a
source file without `extern`, when every whole-word occurrence of the old name
in code (the node tree-sitter gives for those bytes isn't a comment or a
literal) in its region is one of:
- the declaration itself;
- an unqualified identifier that isn't a declarator;
- a type name the grammar can't tell from a value: a lone template argument
  (`f<kSize>()`), or a block's `T name(kSize);`, which it reads as a function
  declaration;
- a member (`p.kSize`, `.kSize =`), or a name qualified by another scope
  (for a namespace constant, a qualifier other than one of the namespaces
  around it; for a block's, any qualifier): not the constant, left alone.

A block's region runs from the declaration to its block's (or statement's)
end; a namespace constant's is the whole file, and any occurrence above it
drops the fix. Anything else (another declaration of the name, a macro's
text, a member declared with the name) drops it too. The new name is taken
when it's written in code anywhere in the file, or when another flagged
constant of the file, under another old name, gets the same new one (both
fixes in one `--fix` pass would clash).

### nested-ternary

From each `conditional_expression`, up through its parents: another
`conditional_expression` reports the inner one; a `lambda_expression`, a
declaration, a statement, a function definition or the file stops the climb.

### else-after-return

An `if_statement` that is a statement of a block or a `case`, without
`constexpr` or `consteval`, with an else, whose branch ends in a jump
(`endingJump()`: a `return`, `continue`, `break` or `throw` statement, a
block whose last statement does, comments aside, or an `if`/`else` whose
branches both do). An `if` that is another's `else if` isn't reported (as
clang-tidy's readability-else-after-return): fixing the outer one makes it a
statement of the block, and it's reported then. The warning is on the `else`
keyword.

The fix replaces everything from the branch's end (or the end of a comment on
the branch's last line) to the else's end with one blank line and the else's
code. The if's indent is its line's leading whitespace (the fix needs the if
to start its line); the moved code's base indent is its first line's, and
each line starting with it gets the if's instead, so whatever indent step the
file uses goes; a line indented less loses up to the difference (a
preprocessor line at column 0 stays). Blank lines stay blank, and line breaks
are the file's (`\r\n` too). A block's braces go; a one-line block's code
becomes one line; an empty block leaves nothing. A statement on the else's
own line (an `else if`) moves as it is, its later lines unchanged, as an
`else if` chain is already indented as the if.

There's no fix when the if or anything in it has a parse error, a comment is
in the else clause or between the branch and the else other than on the
branch's last line, something follows the else block's closing brace on its
line, the else block has code on its opening brace's line and more lines
after it, or the else holds a string literal of several lines. The else's
declarations (the names its top-level statements declare: variables,
functions, types, aliases) must not be declared before the if in its block,
by the statement owning the block (anything declared outside its blocks: a
function's or lambda's parameters, a loop's, if's or catch's variables), or
written anywhere in the block after the if; in a `case` they can't move at
all (a jump to a later label would cross them). The names the if's condition
declares mustn't be written in the else.

## The formatter

`sfs-lsp --format` and the server's `textDocument/formatting` and
`rangeFormatting` share `formatSource()` (`format.cpp`): clang-format, then
the house passes under a `.sfs-format`, then clang-format again if the passes
moved anything (moved lines may need their trailing comments aligned again).
The passes work on the clang-formatted text's tree, so they see its layout:
one declaration per line, braces on lines of their own.

### clang-format

`runProcess()` (`runProcess.cpp`) runs it with the text on stdin,
`--assume-filename=<file>` (so it finds the file's `.clang-format` and
language) and `--fallback-style=none`, reading stdout and stderr as it writes
(a poll loop on POSIX, threads on Windows), so neither pipe can fill and
block it; it's killed after 20 seconds. On POSIX, SIGPIPE is blocked for the
calling thread meanwhile, so a clang-format that dies early gives EPIPE rather
than ending the server, and the child starts with the default signal state.
The file is named `runProcess` because MinGW's C runtime has a `process.h`
that a `process.h` in the server's include folder would hide.

### Entries

Both ordering passes (`formatOrder.cpp`) cut a body (a class body, the file,
a namespace body) into entries with `splitEntries()`: each named child with
the comments above it (back to the previous entry, blank lines between kept
inside the entry) and a comment on its last line. An entry is whole lines:
two things on one line, a declaration on a brace's line, or a stray token make
the body keep its order. Comments after a body's last entry are a node-less
entry that stays last. A divider comment on its own line (`// ---`, `// ===`)
is an entry of its own: a barrier in a scope, and in a class body a reason to
keep the class's order. A label's colon and a class's semicolon belong to the
entry before them. `Rewrite` (`formatLines.cpp`) holds each reordered body's
new text by row range; bodies are done innermost first, and an outer body's
entries are rendered through the inner replacements, which its own then
replaces.

A body that "doesn't parse" has an ERROR node or a missing token
(`parsesBadly()`); a missing name doesn't count, since tree-sitter-cpp reads
a default argument `= {}` as a compound literal missing its type, and those
are everywhere.

### Class bodies

`classify()` sorts each entry into the head (nested types, aliases, using
and friend declarations, static_asserts), methods (a declarator that reaches
a function declarator through pointers and references; a function pointer's
is in parentheses, so it's a field) and fields (static or not). Constructors
are methods named after the class with no return type. A type defined with a
field of it (`struct Walk {...} walk;`) is that field, and declares the type's
name. Anything else (a macro, which tree-sitter reads as a field with a
missing semicolon or as an ERROR) makes the class keep its order.

Two checks guard what the compiler would catch late or not at all:
- without `sort-fields`, the non-static fields must come out in their order;
- `collectUses()` gathers the names each entry's signature uses (not function
  bodies, constructor initialisers or a non-static member's initialiser, which
  see the whole class), and no entry may end up above one it came after whose
  declared names (types, aliases, enumerators of an unscoped enum, static data
  members) it uses.

Blank lines: neighbours that were neighbours (same section, next in order)
keep their separator; new neighbours in a group are separated as most of the
group's old neighbour pairs were (else the section's). Groups are separated
by one blank line, labels by one blank line before them.

### Definitions

The headers come from `headersOf()`: the file's stem with a header extension
beside it, then each quoted include resolved against the file's folder, each
folder above it and that folder's `include/` (as `engine/src/x.cpp` finds
`engine/include/...`), at most 16. `collectDeclared()` reads each one's
classes (their nesting path, and the methods with a virtual specifier), the
namespaces and the free function declarations, then the source's own.

A qualified definition's scope is matched against the classes by the longest
path suffix (`sfs::lsp::Server::run` matches `Server`). A scope that's a
namespace a header declares holds free functions; a scope with a constructor,
destructor or engine hook defined in the file is taken as a class (overrides
are then the known hooks); anything else makes the file keep its order.

Barriers split a body into segments: preprocessor lines, divider comments,
namespaces (ordered on their own), linkage blocks, using-declarations and directives, namespace
aliases, explicit specialisations and instantiations, and statements at file
scope. An `#if` or `#ifdef` among a scope's entries makes the whole file keep
its order: its branches may define the same thing twice.

Pinning: a free function stays in the head when an entry that moves from
after it to before it uses its name and no declaration of it is known
(`Known::functions`: the headers', and the file's own forward declarations);
repeated until nothing changes, since a pinned helper moving up may need
another. A definition with a deduced return type, or constexpr, used by an
entry that moves above it makes the file keep its order, and so does a free
function (a pinned helper, or a head entry's declaration) moving above an
entry that came before it and uses its name: that entry's calls would see an
overload they didn't. Names are compared as identifiers, so a member called
like a helper pins it too: harmless.

### Blank lines round control blocks

`addControlBlankLines()` (`formatBlankLines.cpp`) walks every compound
statement and case: a control statement's unit is it and the comments
directly above it (each on its own line); a blank line goes above the unit
when the thing before it ends on the line just above, and below the statement
(after a comment on its last line) when the next thing starts on the next
line. Preprocessor neighbours and badly parsed ones are left alone, and range
formatting keeps only the rows in its range.

### Safety and edits

`sameLines()` compares the passes' input and output as sorted lists of
non-blank lines without trailing whitespace, access labels left out. The
editor gets `lineEdits()`: a Myers diff of the lines (after the common prefix
and suffix), each hunk a TextEdit at line starts, so positions need no
encoding but at the end of a file without a final line break. Past 1000 edit
steps (a whole-file reorder) it's one hunk.

Timings of `sfs-lsp --format --check` on one file (WSL, clang-format 17.0.6,
an `-Og` build), whole and without clang-format (the house passes, the parses
and the headers read): a game's 4884-line source 0.56 s (0.21 s), a 2507-line
one with many headers 0.40 s (0.20 s), the 4390-line `entityLints.cpp` 0.25 s
(0.13 s), a 587-line header 0.13 s (0.04 s).
