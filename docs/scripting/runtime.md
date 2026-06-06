# Runtime & safety

Running Lua against the live game, and the guarantees around it. For building the
API see the [modding API](./modding-api.md).

## In-app dev console (built in)

A one-line console — toggled with the **backtick** (`` ` ``) key — runs commands
against the live VM without a rebuild. It draws through the engine's `TextRenderer`
(no ImGui dependency), so it ships in **every** build: native (debug or release)
and web.

It is **opt-in**, since it needs a VM to run against: call `setConsoleEnabled(true)`
once after `init()` (see the [overview](./overview.md#quick-start)). The host then
exposes it automatically — no per-frame code.

While open it owns the keyboard (the game ignores input as you type):

| Key | Action |
| --- | --- |
| `` ` `` | toggle the console |
| Enter | run the line (result / error shows above the prompt) |
| Up / Down | recall previous commands |
| Backspace | edit |
| Escape | close |

Anything bound onto the VM is callable here — your bound globals, the engine
tables (`particles.spawn("embers", 10, 8)`), and any [live config](./live-config.md)
table (`weather.set{ fogDensity = 0.5 }`).

> The flag is read every frame, so it can be flipped live. On web the console and
> the on-page editor share the **same** VM, so a value or binding set in one is
> visible in the other.

## Web editor

The page's Lua console (a CodeMirror editor) sends source to the VM via the
exported `sfsEvalLua`; autocomplete comes from `sfsLuaKeys`. Run with
Ctrl/Cmd-Enter or the Run button. (Click the canvas to give input back to the game
and the in-app console; click the editor to type into it.)

## Driving eval yourself

You can call `LuaScripting::eval` / `evalRepl` from your own code (a custom
overlay, a hotkey, a test) — that's all the console and web editor do under the
hood.

```lua
-- an example session against a game's API
particles                          -- dump an API tree
particles.options                  -- what configure() understands
particles.configure("blood_spray", { burst = 60, color = sfs.colors.Lime })
particles.spawn("embers", 10, 8)
weather.set{ windSpeed = 4, fogDensity = 0.5 }  -- live-edit an ILuaConfigurable
```

## Safety & sandboxing

The VM is hardened for an interactive console: the common denial-of-service
vectors (infinite loops, memory exhaustion, output flooding) and filesystem escape
are all bounded — a stray or hostile script cannot hang, OOM, or read the disk.
It's still safest to treat the console as a dev/owner tool rather than an open
endpoint for anonymous code.

- **Execution guard.** Every `eval` / `evalRepl` runs under an instruction budget
  (a Lua count hook). A runaway chunk (`while true do end`) is aborted with an
  error instead of hanging the thread — critical on web, where it would freeze the
  tab. Tune with `setInstructionLimit(n)` (`<= 0` disables).
- **Memory cap.** A byte-accounting allocator fails an over-budget allocation as a
  (caught) out-of-memory error, so a script can't exhaust host RAM. Tune with
  `setMemoryLimit(bytes)` (default 64 MiB, `0` = unlimited); `memoryUsed()` reports
  live bytes.
- **Output cap.** `evalRepl`'s returned string (captured print/log + value) is
  truncated past `setOutputLimit(bytes)` (default 256 KiB) with a marker.
- **C++ exceptions** thrown by your bound functions / config hooks are caught at
  the C boundary and turned into Lua errors (a raw exception crossing Lua's C
  frames is UB). Your game code can throw without crashing the VM.
- **Sandbox.** Only safe stdlib is opened (no `io` / `os` / `package`), and `load`
  / `loadfile` / `dofile` are removed (they reach the filesystem or load raw
  bytecode). Add a text-only `load` wrapper yourself if a script needs it.
- **Single-threaded.** A `lua_State` is not thread-safe; only drive eval from the
  main thread (where the game loop and the web editor entry already run).

## Lifetimes and ownership

- **`LuaScripting`** — app-lifetime. Owned by your `Game` subclass.
- **`ILuaApi` (your adapter)** — *transient*. `registerBindings` runs once; the
  closures it leaves are owned by the VM. Stack-allocate it in `setupLua`. Capture
  the **game** by reference in your lambdas, never the `ILuaApi` object — nothing
  should depend on its lifetime.
- **`ILuaConfigurable`** — outlive the VM, *or* `unregisterConfig()` before
  destruction (the table's slot is then invalidated, so refs no-op). See
  [live config](./live-config.md#lifetime).
- **Don't re-open `namespace sfs`** in game code to forward-declare engine types.
  Include the engine header that declares them. The game only ever *consumes*
  `sfs::`.
