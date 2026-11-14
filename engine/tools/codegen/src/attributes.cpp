#include "sfs/codegen/attributes.h"

#include "sfs/codegen/generator.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace sfs::codegen
{

namespace
{

using enum AttributeId;
constexpr AttributeTarget F = AttributeTarget::Field;
constexpr AttributeTarget T = AttributeTarget::Type;

constexpr std::array<AttributeInfo, 12> kAttributes = {{
    {Range,
     "range",
     F,
     AttributeArgs::MinMax,
     {"Int", "UInt", "Float"},
     "(min, max)",
     "(${1:min}, ${2:max})",
     "Edit as a slider between `min` and `max`.",
     "[[sfs::range(10, 120)]] float fieldOfView = 46.0f;"},
    {Color,
     "color",
     F,
     AttributeArgs::None,
     {"Vec3", "Vec4"},
     "",
     "",
     "Edit with a colour picker.",
     "[[sfs::color]] glm::vec3 color{1.0f, 0.85f, 0.6f};"},
    {Hide,
     "hide",
     F,
     AttributeArgs::None,
     {},
     "",
     "",
     "Don't reflect this public field.",
     "[[sfs::hide]] int debugCounter = 0;"},
    {Show,
     "show",
     F,
     AttributeArgs::None,
     {},
     "",
     "",
     "Reflect this private or protected field.",
     "private:\n"
     "  [[sfs::show]] float waterLevel = 0.5f;"},
    {Script,
     "script",
     F,
     AttributeArgs::None,
     {"String"},
     "",
     "",
     "Edit as a picker of game object scripts.",
     "[[sfs::script]] std::string script;"},
    {Texture,
     "texture",
     F,
     AttributeArgs::None,
     {"String"},
     "",
     "",
     "Edit as a picker of the project's texture assets, saved as "
     "`asset:<id>`.",
     "[[sfs::texture]] std::string normalMap;"},
    {Materials,
     "materials",
     F,
     AttributeArgs::None,
     {"StringList"},
     "",
     "",
     "Edit as a material picker for each section of the component's mesh "
     "field.",
     "[[sfs::materials]] std::vector<std::string> materials;"},
    {Material,
     "material",
     F,
     AttributeArgs::None,
     {"String"},
     "",
     "",
     "Edit as a picker of the project's material assets, saved as "
     "`asset:<id>`.",
     "[[sfs::material]] std::string material;"},
    {EmissionLight,
     "emissionLight",
     F,
     AttributeArgs::None,
     {},
     "",
     "",
     "A setting of the light the component's glowing materials give off: the "
     "Inspector shows it only while one of its materials can glow.",
     "[[sfs::emissionLight]] bool lightCastsShadows = true;"},
    {ShowIf,
     "showIf",
     F,
     AttributeArgs::BoolField,
     {},
     "(field)",
     "(${1:field})",
     "The Inspector shows it only while the named `bool` field of the same "
     "object is on; `showIf(!field)` shows it only while that field is off.",
     "bool orthographic = false;\n"
     "[[sfs::showIf(orthographic)]] float orthoHeight = 20.0f;\n"
     "[[sfs::showIf(!orthographic)]] float fieldOfView = 46.0f;"},
    {Part,
     "part",
     F,
     AttributeArgs::String,
     {"Entity", "EntityList"},
     "(\"node-name\")",
     "(\"${1:node-name}\")",
     "The model node the editor fills it from, by name; on a list, a glob "
     "pattern (`\"blade-*\"`) for every node it takes. In development, a "
     "reference to a node not named so logs a warning. Never used at "
     "runtime.",
     "[[sfs::part(\"deck\")]] sfs::Entity deck;\n"
     "[[sfs::part(\"blade-*\")]] std::vector<sfs::Entity> blades;"},
    {Component,
     "component",
     T,
     AttributeArgs::None,
     {},
     "",
     "",
     "Register the type as a component besides the file's type.",
     "struct [[sfs::component]] TipBody {};"},
}};

constexpr bool inOrder()
{
  for (std::size_t i = 0; i < kAttributes.size(); ++i)
    if (static_cast<std::size_t>(kAttributes[i].id) != i)
      return false;
  return true;
}
static_assert(inOrder(), "kAttributes must list every AttributeId in order");

} // namespace

std::span<const AttributeInfo> attributes() { return kAttributes; }

const AttributeInfo* findAttribute(std::string_view name)
{
  for (const AttributeInfo& info : kAttributes)
    if (info.name == name)
      return &info;
  return nullptr;
}

const AttributeInfo* closestAttribute(std::string_view name)
{
  const AttributeInfo* best = nullptr;
  std::size_t bestDistance = 3;
  for (const AttributeInfo& info : kAttributes)
  {
    const std::size_t distance = editDistance(name, info.name);
    if (distance < bestDistance)
    {
      best = &info;
      bestDistance = distance;
    }
  }
  return best;
}

bool appliesTo(const AttributeInfo& info, std::string_view kind)
{
  if (info.kinds[0].empty())
    return true;
  return std::find(info.kinds.begin(), info.kinds.end(), kind) !=
         info.kinds.end();
}

std::string attributeFieldTypes(const AttributeInfo& info,
                                std::string_view quote)
{
  std::size_t count = 0;
  while (count < info.kinds.size() && !info.kinds[count].empty())
    ++count;
  std::string out;
  for (std::size_t i = 0; i < count; ++i)
  {
    if (i > 0)
      out += i + 1 == count ? " and " : ", ";
    out += quote;
    out += cppTypeOfKind(info.kinds[i]);
    out += quote;
  }
  return out;
}

std::string attributeDocsTable()
{
  std::string out = "| Attribute | On | Effect | Example |\n"
                    "| --- | --- | --- | --- |\n";
  for (const AttributeInfo& info : kAttributes)
  {
    out += "| `[[sfs::";
    out += info.name;
    out += info.arguments;
    out += "]]` | ";
    if (info.target == AttributeTarget::Type)
      out += "A type";
    else if (info.kinds[0].empty())
      out += "Any field";
    else
      out += attributeFieldTypes(info, "`") + " fields";
    out += " | ";
    out += info.summary;
    out += " | ";
    // One code span per line: a table cell holds no code block.
    std::string_view example = info.example;
    while (!example.empty())
    {
      const std::size_t end = example.find('\n');
      std::string_view line = example.substr(0, end);
      while (!line.empty() && line.front() == ' ')
        line.remove_prefix(1);
      out += "`";
      out += line;
      out += "`";
      if (end == std::string_view::npos)
        break;
      out += "<br>";
      example.remove_prefix(end + 1);
    }
    out += " |\n";
  }
  return out;
}

std::size_t editDistance(std::string_view a, std::string_view b)
{
  // One row of the Levenshtein table at a time.
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j)
    row[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i)
  {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j)
    {
      const std::size_t above = row[j];
      const bool same = std::tolower(static_cast<unsigned char>(a[i - 1])) ==
                        std::tolower(static_cast<unsigned char>(b[j - 1]));
      row[j] = std::min({above + 1, row[j - 1] + 1, diagonal + (same ? 0 : 1)});
      diagonal = above;
    }
  }
  return row[b.size()];
}

} // namespace sfs::codegen
