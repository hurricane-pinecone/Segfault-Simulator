#include "engine/core/util/errorDialog.h"

#include <cstdlib>
#include <mutex>
#include <utility>

namespace sfs
{

namespace
{
std::mutex& dialogMutex()
{
  static std::mutex mutex;
  return mutex;
}

ErrorDialog& installed()
{
  static ErrorDialog dialog;
  return dialog;
}
} // namespace

void setErrorDialog(ErrorDialog dialog)
{
  const std::lock_guard lock(dialogMutex());
  installed() = std::move(dialog);
}

bool errorDialogsAllowed()
{
  return std::getenv("SFS_NO_ERROR_DIALOG") == nullptr;
}

bool showErrorDialog(const std::string& message)
{
  if (!errorDialogsAllowed())
    return false;
  ErrorDialog dialog;
  {
    const std::lock_guard lock(dialogMutex());
    dialog = installed();
  }
  if (!dialog)
    return false;
  dialog(message);
  return true;
}

} // namespace sfs
