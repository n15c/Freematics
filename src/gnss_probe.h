// Diagnostics for GNSS assistance (development branch):
// checks whether UBX commands reach the receiver and whether its UBX
// responses can be read back - the prerequisite for saving/restoring the
// navigation database (UBX-MGA-DBD) across power cycles.
#pragma once
#include <FreematicsPlus.h>

// request a probe (web UI); runs in the main loop
void gnssProbeRequest();
// call from the main loop (owns the co-processor link)
void gnssProbeService(FreematicsESP32* sys);
