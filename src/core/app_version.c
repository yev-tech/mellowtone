#include "app_version.h"

/* BUILD_STAMP remains a fixed-width machine value once consumed by the old X1600 bootloader
 * when it compares internal and SD-card builds. A release build sets APP_RELEASE_LABEL
 * (-DRELEASE_LABEL=..., for example "0.1.0"); other builds use the explicit
 * fallback below. */
#ifndef APP_RELEASE_LABEL
  #define APP_RELEASE_LABEL "Development Build"
#endif

const char * app_version_label(void) {
#ifdef TEST_BUILD_TAG
    return "Test Build " TEST_BUILD_TAG;
#else
    return APP_RELEASE_LABEL;
#endif
}

const char * app_build_identifier(void) {
#ifdef TEST_BUILD_TAG
    return TEST_BUILD_TAG;
#elif defined(BUILD_STAMP)
    return BUILD_STAMP;
#else
    return "unknown";
#endif
}
