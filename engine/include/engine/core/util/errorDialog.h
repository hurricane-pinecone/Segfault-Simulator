#pragma once

#include <functional>
#include <string>

namespace sfs
{

/**
 * How the host shows an error to the person at the screen and waits until
 * they dismiss it. The runtime's Game installs one (an SDL message box) when
 * it starts; engine-core has none of its own.
 */
using ErrorDialog = std::function<void(const std::string& message)>;

/**
 * Installs the host's error dialog, replacing any; an empty one removes it.
 * @param dialog the host's dialog
 */
void setErrorDialog(ErrorDialog dialog);

/**
 * Whether error dialogs may be shown: false when the environment sets
 * SFS_NO_ERROR_DIALOG (an automated run, where nobody would click one), so
 * errors stay log lines.
 * @return whether a dialog may be shown
 */
bool errorDialogsAllowed();

/**
 * Shows `message` in the host's error dialog and returns once it's
 * dismissed. Does nothing when no host has installed a dialog or dialogs
 * aren't allowed (errorDialogsAllowed()). It doesn't log: callers log first.
 * Used by a game's fatal error and by a failing sfs::expect in development.
 * @param message what to show
 * @return whether a dialog was shown
 */
bool showErrorDialog(const std::string& message);

} // namespace sfs
