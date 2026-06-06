# Live config

For a single object with tunable fields, implement `sfs::ILuaConfigurable` and the
VM **auto-generates** a `<name>` table — no Lua code at all. For the broader API
see the [modding API](./modding-api.md).

The generated table:

```
<name>.get()      -> table of current values
<name>.set{ ... } -> apply edits, then onLuaConfigChanged()
<name>.options    -> the field schema (key -> hint), for autocomplete
```

## Implementing it

```cpp
class Weather : public sfs::System, public sfs::ILuaConfigurable
{
public:
  std::string luaConfigName() const override { return "weather"; }

  sfs::LuaSchema luaConfigSchema() const override
  {
    return {
        sfs::field("enabled",    &Weather::m_enabled,    "bool"),
        sfs::field("windSpeed",  &Weather::m_windSpeed,  "number"),
        sfs::field("fogDensity", &Weather::m_fogDensity, "0..1"),
    };
  }

  void* luaConfigData() override { return this; } // schema offsets apply here

  // set{} writes raw fields (bypassing setters) -- re-validate / react here.
  void onLuaConfigChanged() override { clampAndApply(); }
};
```

## Registering and unregistering

Register where the object lives. A scene reaches the app-owned VM via
`sfs::activeLua()`, and you **unregister in the owner's destructor** so a torn-down
object can't be reached from Lua:

```cpp
// after creating the object (e.g. in a scene's onInit)
auto& weather = addSystem<Weather>();
if (sfs::LuaScripting* lua = sfs::activeLua())
  lua->registerConfig(weather);

// in the object's destructor
~Weather() override
{
  if (sfs::LuaScripting* lua = sfs::activeLua())
    lua->unregisterConfig(*this); // nils the table; lingering refs no-op
}
```

`unregisterConfig` invalidates the table's backing slot, so even Lua code that
stored a reference (`local w = weather`) safely no-ops afterwards. At app shutdown
`activeLua()` is already null (the VM dies first), making the destructor call a
harmless no-op.

## Schema field builders

From `<engine/core/scripting/luaSchema.h>`:

| Builder | Lua shape | C++ member |
| --- | --- | --- |
| `field(name, &T::m, hint)` | scalar | `int` / `float` / `bool` / `glm::vec2` |
| `rangeField(name, &T::m, hint)` | `nameMin` / `nameMax` | a `{float min, max}` (e.g. `FloatRange`) |
| `colorField(name, hint)` | `sfs.colors` value | applied by you (maps to your own type) |

## Lifetime

The table reaches the object through an invalidatable slot. Either let the object
**outlive the VM**, or call `unregisterConfig()` **before destroying it** (as
above). Forgetting both is the one way to dangle. (Contrast with `ILuaApi`, which
is transient — see [runtime & safety](./runtime.md#lifetimes-and-ownership).)
