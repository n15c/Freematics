// Web UI for configuration and live status (replaces the BLE app).
#pragma once
#include <FreematicsPlus.h>

void webBegin();
// true while a firmware upload is running or a restart is pending:
// the device must not enter standby (WiFi would be switched off)
bool webBusy();
// a web UI request was handled within the last ms milliseconds
bool webRecentlyUsed(uint32_t ms);
