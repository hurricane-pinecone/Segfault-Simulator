#pragma once

#include <string>

// Small ImGui helpers shared by the editor's panels.
namespace widgets
{

/** ImGui::InputText over a std::string, growing it as the user types. */
bool inputText(const char* label, std::string& value, int flags = 0);

} // namespace widgets
