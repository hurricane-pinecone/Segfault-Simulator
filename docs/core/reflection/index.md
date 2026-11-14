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
  type](../../editor/scripting/index.md#scriptable-objects). A type deriving
  from another reflected type (`struct TractorSpec : sfs::VehicleSpec`) is
  that type's kind with the base's fields first (`TypeInfo::base`, each
  `FieldInfo::owner`, `TypeRegistry::isA()` / `chain()`); for now only
  scriptable objects may. A type with no
  base is a component: always for a `struct`, and for a `class` once it has a
  public field. A `class` with no base and no public field (a plain helper
  your scripts use) is left alone, not reflected.
- **Fields:** public data members are reflected, in the same way Unity
  serializes public fields. Static, `const` and function members are ignored.
- **More components in one header:** any other class or struct at namespace
  scope marked `[[sfs::component]]` is a component too, so one header can
  declare a concern's small components together. A header may hold only
  marked types (no type named after the file needed). The same rules apply to
  their fields, and a marked class is a component even with nothing in it. A
  marked type can't have a base (components aren't extended by inheritance:
  that's an error), and one nested in a class or a function, or a template, is
  warned about and not registered. Two types with the same name, in one header
  or across a build's headers, are an error.

```cpp
// TipParts.h: the parts a tipping trailer's model is made of.
namespace game
{
struct [[sfs::component]] TipBody {};
struct [[sfs::component]] TipPivot
{
  float maxAngle = 50.0f;
};
} // namespace game
```

### Tags

A component with no data, an empty struct such as `TipBody` above or the
engine's `sfs::SolidObject`, is a **tag**: it marks an entity (`TypeInfo::tag`,
from `std::is_empty_v`). It is still a component everywhere (saved as
`"TipBody": {}`, queried and added like any other); only the editor treats it
differently, listing it as a chip under the Inspector's Tags. A component with
only `[[sfs::hide]]` fields isn't empty, so it isn't a tag.

### Attributes

Attributes are standard C++ `[[...]]` attributes in the `sfs::` namespace. The
compiler ignores them: the engine passes GCC `-Wno-attributes=sfs::`, which
silences unknown attributes in that namespace only, and Clang
`-Wno-unknown-attributes`, which silences every unknown attribute (Clang has no
switch for one namespace). sfs-codegen checks the `sfs::` ones, and
[sfs-lsp](../../editor/sfs-lsp/index.md) checks them and the standard ones as
you type.

This table is generated from sfs-codegen's attribute table, which sfs-lsp's
completion and hovers also read:

<!-- sfs-attributes:begin -->
| Attribute | On | Effect | Example |
| --- | --- | --- | --- |
| `[[sfs::range(min, max)]]` | `std::int32_t`, `std::uint32_t` and `float` fields | Edit as a slider between `min` and `max`. | `[[sfs::range(10, 120)]] float fieldOfView = 46.0f;` |
| `[[sfs::color]]` | `glm::vec3` and `glm::vec4` fields | Edit with a colour picker. | `[[sfs::color]] glm::vec3 color{1.0f, 0.85f, 0.6f};` |
| `[[sfs::hide]]` | Any field | Don't reflect this public field. | `[[sfs::hide]] int debugCounter = 0;` |
| `[[sfs::show]]` | Any field | Reflect this private or protected field. | `private:`<br>`[[sfs::show]] float waterLevel = 0.5f;` |
| `[[sfs::script]]` | `std::string` fields | Edit as a picker of game object scripts. | `[[sfs::script]] std::string script;` |
| `[[sfs::texture]]` | `std::string` fields | Edit as a picker of the project's texture assets, saved as `asset:<id>`. | `[[sfs::texture]] std::string normalMap;` |
| `[[sfs::materials]]` | `std::vector<std::string>` fields | Edit as a material picker for each section of the component's mesh field. | `[[sfs::materials]] std::vector<std::string> materials;` |
| `[[sfs::material]]` | `std::string` fields | Edit as a picker of the project's material assets, saved as `asset:<id>`. | `[[sfs::material]] std::string material;` |
| `[[sfs::emissionLight]]` | Any field | A setting of the light the component's glowing materials give off: the Inspector shows it only while one of its materials can glow. | `[[sfs::emissionLight]] bool lightCastsShadows = true;` |
| `[[sfs::showIf(field)]]` | Any field | The Inspector shows it only while the named `bool` field of the same object is on; `showIf(!field)` shows it only while that field is off. | `bool orthographic = false;`<br>`[[sfs::showIf(orthographic)]] float orthoHeight = 20.0f;`<br>`[[sfs::showIf(!orthographic)]] float fieldOfView = 46.0f;` |
| `[[sfs::part("node-name")]]` | `sfs::Entity` and `std::vector<sfs::Entity>` fields | The model node the editor fills it from, by name; on a list, a glob pattern (`"blade-*"`) for every node it takes. In development, a reference to a node not named so logs a warning. Never used at runtime. | `[[sfs::part("deck")]] sfs::Entity deck;`<br>`[[sfs::part("blade-*")]] std::vector<sfs::Entity> blades;` |
| `[[sfs::component]]` | A type | Register the type as a component besides the file's type. | `struct [[sfs::component]] TipBody {};` |
<!-- sfs-attributes:end -->

An attribute is checked where it's written. One sfs-codegen doesn't know, one
on the wrong target (a field attribute on a type, or the reverse), one whose
arguments don't fit it (`[[sfs::range(1)]]`), and one on a field type it
doesn't apply to (`[[sfs::color]]` on a `float`) are warnings, and the attribute
is ignored. Two warnings keep the attribute: arguments given to one that takes
none (`[[sfs::hide(1)]]` still hides), and a `showIf` naming no reflected `bool`
field of the same type (the Inspector then shows the field).

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
<<<<<<< Updated upstream
| `sfs::AssetRefList<T>` | List editor (add, remove and pick a scriptable object of type `T` for each row); saved as a JSON array of `asset:<id>` |
=======
| `sfs::Entity` | Another entity (a component only); saved as a path and resolved when the scene or prefab loads (see [Entity references](../../runtime/serialization/index.md#entity-references)) |
| `std::vector<sfs::Entity>` | A list of them (a component only) |
>>>>>>> Stashed changes
| `std::vector<std::string>` | Not shown unless an attribute says how; saved as a JSON array |
| `nlohmann::json` | Not shown; saved as-is (free-form data, such as a script component's per-entity field values) |

A field of any other type, including pointers, references and containers, is
skipped. The build prints a warning that names the field and its type.

Entity fields are for components only: a scriptable object is shared by many
entities and can't point at one, and a script's field can't yet, so on those
the field is skipped with a warning. Inside `namespace sfs`, `Entity` and
`std::vector<Entity>` are read as `sfs::Entity`; elsewhere spell it
`sfs::Entity`. An `std::array` of entities isn't supported.
`TypeRegistry::referencingTypes()` lists the component types with an entity
field, and `reflect::isEntityField(field)` tells one.

## Diagnostics

`sfs-codegen` reports problems in `file:line: severity: message [rule]` form, so
editors and IDEs can jump to them:

```
scripts/Boss.h:3: error: Monster<int> isn't a base a script type can have. Derive from sfs::GameObject, sfs::ui::Layer, sfs::System, sfs::MeshSource, sfs::ScriptableObject or another reflected type, or from nothing for a component. [script-base]
scripts/Crop.h:9: warning: The editor can't edit fields of type std::vector<int> yet, so the field items is skipped. Use a type the editor can edit. [field-type]
scripts/Crop.h:12: warning: There is no attribute [[sfs::hdie]]. Use [[sfs::hide]] if that's what you meant. [unknown-sfs-attribute]
```

An error fails the build. A warning never does: the field or attribute it
names is skipped, or kept where the warning above says so.
In the editor's Code panel, sfs-lsp shows the same diagnostics as you type.

The rule at the end is what an `sfs-ignore` comment names to hide a warning:
`// sfs-ignore-file(no-script-type)` below `#pragma once` quietens a helper
header that declares no script type. Errors can't be hidden. See
[Code checks: sfs-lsp](../../editor/sfs-lsp/index.md#sfs-ignore) for the three
forms and every rule.

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
