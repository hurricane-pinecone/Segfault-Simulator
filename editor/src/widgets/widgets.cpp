#include "widgets/widgets.h"

#include "imgui.h"

namespace widgets
{

namespace
{
// ImGui asks to grow the buffer as the text outgrows it.
int resizeCallback(ImGuiInputTextCallbackData* data)
{
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
  {
    auto* value = static_cast<std::string*>(data->UserData);
    value->resize(static_cast<std::size_t>(data->BufTextLen));
    data->Buf = value->data();
  }
  return 0;
}
} // namespace

bool inputText(const char* label, std::string& value, int flags)
{
  return ImGui::InputText(label,
                          value.data(),
                          value.capacity() + 1,
                          flags | ImGuiInputTextFlags_CallbackResize,
                          resizeCallback,
                          &value);
}

} // namespace widgets
