// Tests for the particle effect reflection schema: it exposes the Lua-editable
// fields with sane offsets and kinds, so the table->desc reader and the
// autocomplete doc are driven from one list.

#include "../../testHarness.h"

#include <engine/core/scripting/luaSchema.h>
#include <engine/core/scripting/particleSchema.h>

#include <cstddef>
#include <string>

using namespace sfs;

namespace
{
const LuaField* find(const LuaSchema& schema, const std::string& name)
{
  for (const LuaField& f : schema)
    if (f.name == name)
      return &f;
  return nullptr;
}
} // namespace

int main()
{
  const LuaSchema& schema = particleEffectSchema();

  CHECK(!schema.empty());

  // a few known fields are present
  CHECK(find(schema, "lifetime") != nullptr);
  CHECK(find(schema, "speed") != nullptr);
  CHECK(find(schema, "drag") != nullptr);
  CHECK(find(schema, "size") != nullptr);
  CHECK(find(schema, "color") != nullptr);

  // kinds are wired correctly: drag is a scalar float, lifetime/speed/size are
  // FloatRange (Range), color is the caller-handled Color kind
  CHECK(find(schema, "drag")->kind == FieldKind::Float);
  CHECK(find(schema, "lifetime")->kind == FieldKind::Range);
  CHECK(find(schema, "speed")->kind == FieldKind::Range);
  CHECK(find(schema, "size")->kind == FieldKind::Range);
  CHECK(find(schema, "color")->kind == FieldKind::Color);

  // names are unique
  bool unique = true;
  for (std::size_t i = 0; i < schema.size(); ++i)
    for (std::size_t j = i + 1; j < schema.size(); ++j)
      if (schema[i].name == schema[j].name)
        unique = false;
  CHECK(unique);

  return testing::report("particleSchemaTests");
}
