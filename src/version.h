// Firmware version and build number (see scripts/build_info.py)
#pragma once
#include <stdint.h>

uint32_t fwBuild();        // build number (git commit count), 0 if unknown
const char* fwCommit();    // short git hash, "+" = uncommitted changes
const char* fwBuildTime(); // UTC build time
// "2.0.0 build 42 (abc1234, 2026-11-17 18:00 UTC)"
const char* fwVersionString();
