// Automatic firmware update from GitHub releases (only over WiFi).
//
// The CI publishes for every build on master a release with
//   firmware-<env>.bin  and  firmware-<env>.json
// {"build":101,"version":"2.0.0","commit":"abc1234","epoch":1763460000,
//  "url":"https://github.com/.../firmware-<env>.bin","size":1371234,"sha256":"..."}
// The device fetches <updateUrl>firmware-<env>.json, installs the image if
// it is newer (commit time) and verifies its SHA-256 before switching.
#pragma once
#include <ArduinoJson.h>

// called from the uplink task; downloads only over WiFi, automatically only
// with the ignition off and at most every few hours
void otaService(bool wifiUp, bool ignition, bool standby);
// web UI: check (and install) now
void otaRequestCheck();
// true while a download/installation runs (no standby)
bool otaBusy();
void otaStatusJson(JsonObject o);
