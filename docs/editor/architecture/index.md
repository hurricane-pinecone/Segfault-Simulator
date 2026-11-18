# Architecture board

A game built from systems is hard to see as a whole: each script reads well on
its own, but what touches what is spread over every file. The **Architecture**
board (**Window → Architecture**) draws it: your systems, services, game
objects, components, scene data, events and utils files as boxes on an endless
grid, and what each one's code does with the others as arrows. Which system
writes `Health`? Who sends `EnemyDiedEvent`, and who consumes it? Which systems
read other systems? The board shows it as the code has it.

It hides nothing to look tidy. A tangle on the board is a tangle in the code,
and it shows you what to clean up: a component written by five systems, a
system everything reads, an event nobody consumes. Instead of pruning, the
board helps you find your way round: pan and zoom, search, light up one box's
neighbours, show one box's neighbourhood alone, or switch kinds of box and
arrow off.

## What it shows

Every box is a type in your scripts (or a utils file), titled with its name,
with its kind under it. Its header's colour is its kind's:

| Box | What it is |
| --- | --- |
| system, service, game object | Code that does the work. |
| component, scene data | The state it works on. |
| event | What one system tells another: its role (`news`, `entityNews`, `request`, `response`) follows the kind. |
| scriptable object, mesh source | Shared data assets. |
| utils | A header and source of free helpers whose code touches components, scene data, events or systems. |

Engine types your code uses (`TransformComponent`) are boxes too, greyed back
and marked `engine`, so no arrow is lost. A type that isn't declared in your
scripts is marked `not declared`.

Every arrow points from the box whose code does something to the box it's
done to, and is coloured by what it does:

| Arrow | From the code |
| --- | --- |
| `registers` | `registerComponent<T>()`: a system's own entities have it. |
| `observes` | `observe<...>()`: it hears entities gain or lose it. |
| `reads` | Const access: `getComponent<const T>()`, a `const T` query term, `sceneData<const T>()`. |
| `writes` | Mutable access: the code changes it. |
| `adds`, `removes` | `addComponent<T>()`, `removeComponent<T>()`, `addSceneData<T>()`... |
| `filters` | `sfs::With`, `sfs::Excluding`, `hasComponent<T>()`: it only checks it's there. |
| `sends` | `send(E{...})`, `broadcast<E>()`. |
| `consumes` | `consumer<E>()`. |
| `requests`, `responds` | A requester asking, a responder answering. |
| `reads-system` | `getSystem<T>()` of another system, which the [`system-reads-system`](../sfs-lsp/index.md#rules) rule warns about. |
| `uses-util` | An `#include` of a utils file's header. |

The [export's documentation](../sfs-lsp/index.md#architecture-export) has
every form each one comes from, and what can't be seen (there's no call graph:
a system calling a utils function shows as `uses-util`, and the utils box
holds the access).

## Reading the layout

The board lays itself out in columns, one for each kind of box, left to
right:

1. **Game objects**.
2. **Scene data**.
3. **Components**.
4. **Systems**.
5. **Services**.
6. **Utils** files.
7. **Events**.
8. **Scriptable objects** and **mesh sources**, a column each.

Every box of a kind is in its kind's column, and only there: a column is as
tall as it needs to be (the board has no edges), and a kind your scripts
don't have takes no room. The state sits between the two kinds of code that
work on it, game objects on one side and systems on the other, and the systems
sit between the data they work on and the utils and events they use. Most
arrows run between neighbouring columns, so they're short: of every order the
columns could go in, this one gives close to the shortest, least bent and
least crossed arrows. Every column's rows line up, with gaps between them.
Within each column, boxes are ordered so they sit level with what they're
joined to in the other columns, which keeps arrows short and crossings few.
The layout is the same each time for the same scripts.

### How the arrows run

Arrows never run under a box. Each leaves its box from a side, runs along the
gutter beside the box's column, along the gap between two rows across the
columns in its way, down the gutter beside its target and into the target's
side, with rounded corners and its arrowhead at the target. Boxes in
neighbouring columns are joined straight across the gutter between them. A box
dragged off the grid gets its own way round it, and arrows go round it rather
than under it.

Arrows from one box travel together, as one trunk, and branch off where they
must; arrows into one box gather the same way. How far apart they are depends
on the zoom:

- **Zoomed out**, each stretch of gutter or gap is drawn once, as a trunk:
  thicker the more arrows it carries, in their kind's colour if most are of one
  kind, grey otherwise. Hover a trunk to see what it carries
  (`HitchSystem: 42 arrows, 5 kinds`, and the arrows), and it lights up with
  every arrow in it, whole.
- **Zooming in**, the trunks spread smoothly apart until each arrow has a lane
  of its own, in its own colour, with those of one box side by side so they
  read as a stream. Hover one to see it alone.

Drag boxes to arrange them as you like (a drag on empty space selects the
boxes it covers, and they move together); the arrows find their way round
them again as you drag. Where you put them is saved per project, in
`.sfs/architecture.json`, and kept when the board refreshes. (Boxes saved
before the layout gave each kind a column of its own are laid out afresh,
once.) A box for a type you've just added is placed in its kind's column,
below anything in the way, without moving yours. **Re-layout** forgets where boxes were moved and
lays them all out afresh.

## Finding your way round

- **Pan** with the middle mouse button, or by dragging with the right one. The
  board has no edges. **Zoom** with the wheel, about the mouse. **F** or
  **Frame** fits the selected boxes, or every box, in view.
- **Hover** a box to light it up with its arrows and the boxes at their other
  ends; everything else dims. **Click** to select it, and the highlight stays
  while the mouse is elsewhere. **Ctrl**+click selects more. **Esc** clears
  the selection.
- **Hover an arrow** to see what it is (`CombatSystem writes Health`) and the
  places it's written, as `file:line`.
- **Neighbourhood** (the toolbar's checkbox, **N**, or double-clicking a box)
  shows the selected box alone with its neighbours round it: what points at it
  on the left, what it points at on the right. Double-click a neighbour to move
  to its neighbourhood; **N** or **Esc** goes back to the whole board.
- **Search** (the box at the top, or **Ctrl+F**): type part of a name. The
  list under it shows the matches, best first (the whole name, then names
  starting with it, then a later word in a name, so `died` finds
  `EnemyDiedEvent`, then anywhere in a name, then the letters in order, so
  `edv` finds it too). **Up** and **Down** choose, and **Enter** or a click
  selects the box and frames it, and puts its name in the search box, which
  keeps it while that box is selected.
- **Open a script** in the Code panel (from the Assets panel, a place listed
  in the Inspector, or Neovim itself, `:edit` or switching buffers) and, while
  the board is open, its box is selected, lit with its arrows and neighbours,
  and framed with them. A system's header and source both lead to it, a utils
  file to its utils box, and a source file of its own that a type's code is
  split into to that type. A script with no box leaves the board as it is, and
  so does one opened while you're typing in the search box. In a
  neighbourhood, the script's box's neighbourhood shows.

The board only shows: it edits nothing, so Undo, Delete and the other Edit
commands do nothing while it's the panel in use.

## Switching kinds off

**Filters**, at the board's top right, opens over the board to show each
kind's colour and how many there are, with a checkbox to hide it; folded, it
says how many kinds are off. Hiding a kind of box hides its
arrows too; hiding a kind of arrow leaves the boxes. **Right-click** a kind to
show it alone (to see only `writes`, say), and right-click it again to show
every kind. Highlights and neighbourhoods follow what's shown.

## Details

Select a box to list, in the **Inspector**, where it's declared and everything
its code does (**What its code does**) and everything done to it (**What's
done to it**), grouped by arrow kind. The Inspector shows it until you select
an entity or asset; selecting or clicking a box on the board shows it again. Each line is the box at the other end
(click it to select that box) and the places it's written, as `file:line`
with the call it's made with: click one to open the file at that line in the
Code panel.

## How the graph is made

The board doesn't keep anything running in the background. When it opens, it
runs [`sfs-lsp --graph`](../sfs-lsp/index.md#architecture-export) on the
project's `scripts/` folder as a separate process, and a bar at the board's
bottom left shows how far it's got (listing, parsing, indexing and linking the
files, with a count). For a game of a few hundred script files that's a second
or two.

While the board is open it runs again when your scripts change: a second after
you save in the Code panel (several saves together make one run), or when you
come back to the editor after editing them elsewhere. **Refresh** runs it
straight away. The board keeps showing the last graph until the new one is in.
Closing the board stops a run that's under way, and nothing runs while it's
closed.

If the graph can't be made (sfs-lsp isn't built, or it fails), the board says
why at its top left and keeps the last graph it had.
