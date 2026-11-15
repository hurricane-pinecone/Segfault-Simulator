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
  `onResponses`), the constructor (its initialiser list included), the
  destructor, and a default member initialiser.

Any other method counts as set-up (`create()`, `onCreate()` or a helper they
call), and there the handle must be kept. The value is followed up from the
call through parentheses, a `?:` branch, braces (`= {...}`), a builder chain
(`requester<R>().onResponse<A>(f).ignore<B>()`), `std::move()` and a handle
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

A send is `x.respond(arg)` or `x.progress(arg)` with one argument whose type
can be read: `R{...}`, `R(...)`, `std::move(v)` or `v` for a local or
parameter `v` declared with a type (for `auto`, the type its value makes) or
a data member declared in the class body around the call. Spellings are
normalised as codegen normalises types (`codegen::normaliseType()`).

Response types come from codegen's event models (`responseTypesOf()`): a
struct marked `[[sfs::event_response]]` declares its qualified name; each
type an `[[sfs::event_request(...)]]` lists gives every name its spelling may
mean from the request's namespaces, as listed only.

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
