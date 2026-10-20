/* Run the real handoff suite with Entware platform selection on a host.
 * Rename the included implementation's exports so the generic test link rule
 * can still link the ordinary backend objects without duplicate symbols. */
#undef MT_PLATFORM_OPENWRT
#ifndef MT_PLATFORM_ENTWARE
#define MT_PLATFORM_ENTWARE 1
#endif
#undef MT_BUILD_TARGET
#define MT_BUILD_TARGET "mipsel-3.4_kn"
#undef MT_VERSION
#define MT_VERSION "0.8.5"
#define MT_TEST_ENTWARE_SHELL 1
#define mt_update_register_routes mt_test_entware_update_register_routes
#define mt_update_shutdown mt_test_entware_update_shutdown
#include "test_update_process.c"
