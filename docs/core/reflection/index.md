# Reflection & codegen

Reflection gives the engine a runtime description of your types: their names,
their fields and the editor hints on those fields. The editor's inspector, scene
files and "Add Component" are all built on it, so a component you write shows up
everywhere without any extra code.

You never write reflection by hand. `sfs-codegen` reads your headers at build
time and generates it.

## Writing a reflected type

There are no macros and no registration calls. A header is picked up when it
declares a class named after the file:

```cpp
// Crop.h
#pragma once
#include <glm/glm/ext/vector_float3.hpp>
#include <string>

struct Crop
{
  [[sfs::range(0, 1)]] float growth = 0.0f;
  [[sfs::color]] glm::vec3 tint{1.0f};
  std::string variety = "wheat";

  [[sfs::hide]] int debugCounter = 0;   // public, but not shown or saved

private:
  [[sfs::show]] float waterLevel = 0.5f; // private, but shown and saved
  float cache = 0.0f;                    // private: ignored
};
```

These rules decide what is reflected:

- **The type:** the class or struct named after the file. The match ignores
  case, so `transformComponent.h` declares `TransformComponent`.
- **What kind of type it is:** a type deriving from `sfs::GameObject` is a game
  object, one deriving from `sfs::System` is a system, one deriving from
  `sfs::MeshSource` is a mesh source, and one deriving from
  `sfs::ScriptableObject` is a [scriptable object
  type](../../editor/scripting/index.md#scriptable-objects). A type with no
  base is a component: always for a `struct`, and for a `class` once it has a
  public field. A `class` with no base and no public field (a plain helper
  your scripts use) is left alone, not reflected.
- **Fields:** public data members are reflected, in the same way Unity
  serializes public fields. Static, `const` and function members are ignored.

### Attributes

Attributes are standard C++ `[[...]]` attributes in the `sfs::` namespace. The
compiler ignores them; the engine turns off GCC's and Clang's unknown-attribute
warning for that namespace.

| Attribute | Effect |
| --- | --- |
| `[[sfs::range(min, max)]]` | Edit as a slider between `min` and `max` |
| `[[sfs::color]]` | Edit a `glm::vec3` or `glm::vec4` with a colour picker |
| `[[sfs::hide]]` | Don't reflect this public field |
| `[[sfs::show]]` | Reflect this private or protected field |
| `[[sfs::script]]` | Edit a `std::string` as a picker of game object scripts |
| `[[sfs::texture]]` | Edit a `std::string` as a picker of the project's texture assets (saved as `asset:<id>`) |
| `[[sfs::materials]]` | Edit a `std::vector<std::string>` as a material picker for each section of the component's mesh field |
| `[[sfs::material]]` | Edit a `std::string` as a picker of the project's material assets (saved as `asset:<id>`) |
| `[[sfs::emissionLight]]` | A setting of the light the component's glowing materials give off: the Inspector shows it only while one of its materials can glow |
| `[[sfs::showIf(field)]]` | The Inspector shows it only while the named `bool` field of the same object is on, as a camera's bloom settings show only while it blooms. `showIf(!field)` shows it only while that field is off. |

`[[sfs::show]]` works on private fields without making your class `friend` the
engine. The generated code reaches the member through an explicit template
instantiation, which the C++ standard exempts from access checks.

### Field types

| C++ type | Inspector widget |
| --- | --- |
| `bool` | Checkbox |
| `int` / `int32_t` | Integer drag (or slider with a range) |
| `unsigned` / `uint32_t` | Unsigned drag |
| `float` | Drag (or slider with a range) |
| `std::string` | Text field |
| `glm::vec2` / `vec3` / `vec4` | Vector drags (or a colour picker) |
| `glm::quat` | Euler angles in degrees |
| `sfs::MeshHandle` | Mesh picker; saved by mesh name |
| `sfs::AssetRef<T>` | Picker of the project's [scriptable objects](../../editor/scripting/index.md#scriptable-objects) of type `T`; saved as `asset:<id>` |
| `std::vector<std::string>` | Not shown unless an attribute says how; saved as a JSON array |
| `nlohmann::json` | Not shown; saved as-is (free-form data, such as a script component's per-entity field values) |

A field of any other type, including pointers, references and containers, is
skipped. The build prints a warning that names the field and its type.

## Diagnostics

`sfs-codegen` reports problems in `file:line: severity: message` form, so editors
and IDEs can jump to them:

```
scripts/Boss.h:3: error: 'Monster' isn't a supported base; derive from sfs::GameObject, sfs::System, sfs::MeshSource or sfs::ScriptableObject, or nothing for a component
scripts/Crop.h:9: warning: 'items' has type 'std::vector<int>', which the editor can't edit yet; skipped
```

An error fails the build. A warning skips only the field it names.

The generated code also checks each field's type at compile time. If the parser
ever misreads a type, for example through a typedef, the build fails with a
`static_assert` that names the field, rather than corrupting memory at runtime.

## Using reflection

`sfs::reflect::types()` returns the registry for the whole process. The engine's
components are already registered in it, and a game module adds its own types
when it loads.

```cpp
#include "engine/core/reflection/reflection.h"

for (const sfs::reflect::TypeInfo& type : sfs::reflect::types().types())
{
  if (type.kind != sfs::reflect::TypeKind::Component || !type.component.has(entity))
    continue;

  void* component = type.component.get(entity);
  for (const sfs::reflect::FieldInfo& field : type.fields)
    if (field.kind == sfs::reflect::FieldKind::Float)
      field.in<float>(component) *= 2.0f;
}
```

Each component type also has type-erased operations: `has`, `get`, `add` and
`remove` on an entity. Systems and game objects are also registered with
`sfs::scripts()`, along with how to create them in a scene; see
[Game scripts](../../editor/scripting/index.md). A type also records the `module` that registered it, and
`removeModule()` drops a module's types before the module is unloaded.

## Generating reflection in CMake

The engine generates reflection for its own components with the same helper a
project uses:

```cmake
sfs_generate_reflection(myGame
    MODULE game                           # stored on each registered type
    FUNCTION game::registerTypes          # function the generated code defines
    FUNCTION_HEADER game/registration.h   # declares that function
    INCLUDE_ROOT "${CMAKE_SOURCE_DIR}/src"
    HEADERS src/Crop.h src/Player.h)
```

`sfs-codegen` runs on the build machine. A native build builds it. A
cross-build, such as Windows binaries built from WSL, uses the native build's
copy instead:

```bash
cmake ... -DSFS_CODEGEN_EXECUTABLE=<native-build>/bin/sfs-codegen
```

The generated file is rewritten only when its contents change, so an
unchanged header doesn't trigger a rebuild.
