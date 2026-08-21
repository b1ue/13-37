#pragma once

// Bump the semantic version when a user-visible firmware set is cut. The
// build hook generates firmware_build.h on every PlatformIO invocation so an
// individual binary can also be tied to its exact Git revision.
#define FW_NAME        "13:37"
#define FW_VERSION     "1.1.0-beta.9"
#define FW_FEATURE_SET "standby-retro-network-r10"

#if __has_include("firmware_build.h")
#include "firmware_build.h"
#else
#define FW_BUILD_ID "source"
#endif
