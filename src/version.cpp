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
#ifndef FW_COMMIT_EPOCH
#define FW_COMMIT_EPOCH 0
#endif
#ifndef FW_ENV
#ifdef BOARD_HAS_PSRAM
#define FW_ENV "freematics_one_plus_psram"
#else
#define FW_ENV "freematics_one_plus"
#endif
#endif

uint32_t fwBuild() { return FW_BUILD; }
const char* fwCommit() { return FW_COMMIT; }
const char* fwBuildTime() { return FW_BUILD_TIME; }
uint32_t fwCommitEpoch() { return FW_COMMIT_EPOCH; }
#ifndef FW_BRANCH
#define FW_BRANCH "unknown"
#endif
const char* fwEnv() { return FW_ENV; }
const char* fwBranch() { return FW_BRANCH; }

const char* fwVersionString()
{
  static char buf[160];
  if (!buf[0]) snprintf(buf, sizeof(buf), "%s build %u (%s, %s, %s)", FW_VERSION, (unsigned)FW_BUILD, FW_COMMIT, FW_BRANCH, FW_BUILD_TIME);
  return buf;
}
