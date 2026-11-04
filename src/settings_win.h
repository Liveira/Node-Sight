// Owned native settings window; drafts do not mutate live collectors until Apply.
#pragma once
#include <windows.h>
#include <functional>
#include "monitor.h"
using SettingsApply = std::function<bool(const Config&)>;
/** Open/focus one modeless settings window. The owner continues receiving collection timers. */
void openNativeSettings(HWND owner, const Config& config, SettingsApply apply,
                        std::function<DashboardState()> status);
/** Forward tab navigation only while the settings window is alive. */
bool settingsMessage(MSG* message);
/** Export this application's anonymous settings window, never the user's desktop. */
bool exportNativeSettings(const std::wstring& path, int page = 0);
