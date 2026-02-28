#pragma once
// settings_dialog.h — Win32 settings dialog for FPS and output directory
// T027: FPS dropdown (30/60), output directory browser, settings persisted via AppSettings

#include <windows.h>
#include "app/app_settings.h"

namespace sr {

// Shows the modal settings dialog.
// Loads current values from `settings` on entry.
// If the user clicks OK, writes back the new values and returns true.
// If the user cancels, `settings` is unmodified and returns false.
bool ShowSettingsDialog(HWND parent, AppSettings& settings);

} // namespace sr
