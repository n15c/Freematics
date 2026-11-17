#include "version.h"
#include <stdio.h>
#include "config.h"

#if __has_include("build_info.h")
#include "build_info.h"
#endif
#ifndef FW_BUILD
#define FW_BUILD 0
#define FW_COMMIT "unknown"
#define FW_BUILD_TIME __DATE__ " " __TIME__
#endif

uint32_t fwBuild() { return FW_BUILD; }
const char* fwCommit() { return FW_COMMIT; }
const char* fwBuildTime() { return FW_BUILD_TIME; }

const char* fwVersionString()
{
  static char buf[80];
  if (!buf[0]) snprintf(buf, sizeof(buf), "%s build %u (%s, %s)", FW_VERSION, (unsigned)FW_BUILD, FW_COMMIT, FW_BUILD_TIME);
  return buf;
}
