#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Forward-declared so this header pulls in no Lua headers -- the schema itself
// is plain data and is safe to include anywhere (it describes a struct, it does
// not touch the VM). The Lua-aware reader functions below take a lua_State* but
// are only defined in the scripting layer.
struct lua_State;

namespace sfs
{

// A field's wire type. Drives how the reader marshals a Lua value to/from the
// member at `offset`. Range maps a FloatRange {min,max} to the `<name>Min` /
// `<name>Max` key pair; Color is caller-handled (the target type and authoring
// convention -- e.g. a Gradient -- are domain-specific), so the reader only
// surfaces it in the schema/options doc and skips it on read/dump.
enum class FieldKind : std::uint8_t
{
  Int,
  Float,
  Bool,
  Vec2,
  Range,
  Color,
};

// One reflected field: the Lua key, the byte offset of the member within its
// struct, its wire type, and a human hint shown in the autocomplete/options doc.
struct LuaField
{
  std::string name;
  std::size_t offset;
  FieldKind kind;
  std::string hint;
};

using LuaSchema = std::vector<LuaField>;

// Byte offset of a member within its struct, computed from a member pointer via
// a real (default-constructed) instance -- well-defined, unlike the null-pointer
// offsetof trick. The struct must be default-constructible.
template <typename C, typename M>
std::size_t memberOffset(M C::*member)
{
  static C probe{};
  return static_cast<std::size_t>(
      reinterpret_cast<const char*>(&(probe.*member)) -
      reinterpret_cast<const char*>(&probe));
}

// Field builders -- the kind is deduced from the member's type, so a schema reads
// as a plain list of (key, member, hint). Composite kinds (Range/Color) have
// their own builders because they don't map one-to-one onto a single Lua scalar.
template <typename C>
LuaField field(std::string name, int C::*member, std::string hint)
{
  return {std::move(name), memberOffset(member), FieldKind::Int, std::move(hint)};
}

template <typename C>
LuaField field(std::string name, float C::*member, std::string hint)
{
  return {
      std::move(name), memberOffset(member), FieldKind::Float, std::move(hint)};
}

template <typename C>
LuaField field(std::string name, bool C::*member, std::string hint)
{
  return {
      std::move(name), memberOffset(member), FieldKind::Bool, std::move(hint)};
}

template <typename C>
LuaField field(std::string name, glm::vec2 C::*member, std::string hint)
{
  return {
      std::move(name), memberOffset(member), FieldKind::Vec2, std::move(hint)};
}

// A FloatRange (or any {float min; float max;} member): read/dumped as the
// `<name>Min` / `<name>Max` key pair. R is deduced, so the FloatRange type need
// not be named here (keeps this header domain-free).
template <typename C, typename R>
LuaField rangeField(std::string name, R C::*member, std::string hint)
{
  return {
      std::move(name), memberOffset(member), FieldKind::Range, std::move(hint)};
}

// A colour field: the reader only documents it (the target type -- e.g. a
// Gradient -- and the authoring convention are domain-specific, so the caller
// reads it via readColor() and applies its own mapping).
inline LuaField colorField(std::string name, std::string hint)
{
  return {std::move(name), 0, FieldKind::Color, std::move(hint)};
}

namespace luaschema
{

// Read every schema field present in the Lua table at `tableIdx` into `obj`
// (missing keys are left at their current value). Color fields are skipped --
// the caller applies its own colour-to-target mapping.
void readTable(lua_State* L, int tableIdx, void* obj, const LuaSchema& schema);

// Push a new table mapping each key to its hint -- the self-describing `options`
// doc used for autocomplete. Range fields expand to `<name>Min` / `<name>Max`.
void pushSchema(lua_State* L, const LuaSchema& schema);

// Push a new table of `obj`'s current values keyed the same way configure reads
// them (Range -> `<name>Min` / `<name>Max`), so a dump round-trips as input.
// Color fields are omitted.
void pushValues(lua_State* L, const void* obj, const LuaSchema& schema);

// Read an sfs.colors-style table at `idx` ({r,g,b,a} 0-255, or the array form
// {255,0,0}) into a 0..1 vec3.
glm::vec3 readColor(lua_State* L, int idx);

} // namespace luaschema

} // namespace sfs
