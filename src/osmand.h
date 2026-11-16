// OsmAnd protocol (HTTP GET query / POST form).
//
// Natively understood by Traccar (port 5055, "osmand" protocol) and by the
// Home Assistant "Traccar Client" integration (webhook). Unknown parameters
// are stored by Traccar as position attributes, so OBD/CAN values travel in
// the same request. Attribute names follow Traccar's standard keys where one
// exists (ignition, motion, power, rssi, sat, hdop, rpm, coolantTemp, ...).
#pragma once
#include "record.h"

// builds "id=...&timestamp=...&lat=...": returns length (0 if it did not fit)
int osmandQuery(const Record& r, const char* deviceId, char* buf, size_t size);
// builds the request URL for GET (query appended) or POST (url only)
int osmandUrl(const char* base, const char* query, bool post, char* buf, size_t size);
