// Connectivity (WiFi station + AP, cellular) and upload of queued records.
// Runs in its own FreeRTOS task; the main loop only pushes records.
#pragma once
#include <FreematicsPlus.h>

void uplinkBegin(FreematicsESP32* sys);
// main loop tells the uplink whether the device is in standby (parked)
void uplinkSetStandby(bool standby);
// use the modem's GNSS (no standalone receiver)
void uplinkUseModemGnss(bool on);
// number of clients connected to the own access point (web UI in use)
int uplinkApClients();
// unique id used in the protocol
const char* deviceId();
// persist queue and restart (used by web UI)
void systemRestart();
