/* OpenWrt must keep using its system shell, not require an Entware prefix. */
#undef MT_PLATFORM_ENTWARE
#ifndef MT_PLATFORM_OPENWRT
#define MT_PLATFORM_OPENWRT 1
#endif
#define mt_update_register_routes mt_test_openwrt_update_register_routes
#define mt_update_shutdown mt_test_openwrt_update_shutdown
#include "test_update_process.c"
