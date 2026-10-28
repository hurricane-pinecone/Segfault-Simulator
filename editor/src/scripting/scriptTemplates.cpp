#include "scripting/scriptTemplates.h"

#include "scripting/scriptFiles.h"

#include "engine/core/logger/logger.h"

#include <cctype>
#include <fstream>

namespace scriptTemplates
{

namespace
{
std::string source(Kind kind, const std::string& name)
{
  switch (kind)
  {
  case Kind::GameObject:
    return "#pragma once\n"
           "#include <sfs/sfs.h>\n"
           "\n"
           "class " + name + " : public sfs::GameObject\n"
           "{\n"
           "protected:\n"
           "  void onCreate(sfs::Scene& scene) override\n"
           "  {\n"
           "  }\n"
           "\n"
           "  void onUpdate(double deltaTime) override\n"
           "  {\n"
           "  }\n"
           "};\n";
  case Kind::System:
    return "#pragma once\n"
           "#include <sfs/sfs.h>\n"
           "\n"
           "class " + name + " : public sfs::System\n"
           "{\n"
           "protected:\n"
           "  void create() override\n"
           "  {\n"
           "    // registerComponent<YourComponent>();\n"
           "  }\n"
           "\n"
           "  void update(double deltaTime) override\n"
           "  {\n"
           "    for (sfs::Entity& entity : getEntities())\n"
           "    {\n"
           "    }\n"
           "  }\n"
           "};\n";
  case Kind::Component:
    return "#pragma once\n"
           "#include <sfs/sfs.h>\n"
           "\n"
           "struct " + name + "\n"
           "{\n"
           "};\n";
  case Kind::ScriptableObject:
    return "#pragma once\n"
           "#include <sfs/sfs.h>\n"
           "\n"
           "// Shared data, kept in .asset files: make one in the Assets panel\n"
           "// (New Scriptable Object), and refer to it with an sfs::AssetRef<" + name + "> field.\n"
           "struct " + name + " : sfs::ScriptableObject\n"
           "{\n"
           "};\n";
  }
  return {};
}
} // namespace

const char* label(Kind kind)
{
  switch (kind)
  {
  case Kind::GameObject:
    return "GameObject Script";
  case Kind::System:
    return "System";
  case Kind::Component:
    return "Component";
  case Kind::ScriptableObject:
    return "Scriptable Object Template";
  }
  return "";
}

bool isValidName(const std::string& name)
{
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
    return false;
  for (const char c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
      return false;
  return true;
}

std::optional<std::filesystem::path> create(Kind kind,
                                            const std::string& name,
                                            const std::filesystem::path& scriptsFolder,
                                            const std::filesystem::path& folder)
{
  if (!isValidName(name))
  {
    LOG_ERROR("'" + name + "' isn't a valid class name");
    return std::nullopt;
  }

  if (const auto existing = scriptFiles::find(scriptsFolder, name, scriptFiles::Prefer::Header))
  {
    LOG_ERROR("There's already a script called " + name + " (" +
              existing->lexically_relative(scriptsFolder).generic_string() + ")");
    return std::nullopt;
  }

  const std::filesystem::path path = folder / (name + ".h");
  std::filesystem::create_directories(folder);
  std::ofstream out(path, std::ios::binary);
  out << source(kind, name);
  if (!out)
  {
    LOG_ERROR("Can't write " + path.string());
    return std::nullopt;
  }

  LOG_INFO("Created " + std::string(label(kind)) + " " + path.filename().string());
  return path;
}

} // namespace scriptTemplates
