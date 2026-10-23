#pragma once

#ifndef appconfAUDIO_CFG_SERVICER_COMPAT_ENABLED
#define appconfAUDIO_CFG_SERVICER_COMPAT_ENABLED 1
#endif

#ifndef appconfLED_RING
#define appconfLED_RING 0
#endif

#ifndef BUILTIN_TESTS_SPI_ECHO_SERVICER
#define BUILTIN_TESTS_SPI_ECHO_SERVICER 0
#endif

/* The chain control resource (src/chain_control), on the speaker-pipeline tile */
#ifndef appconfCHAIN_CONTROL_ENABLED
#define appconfCHAIN_CONTROL_ENABLED 1
#endif

/*
 * Base SQ66/SATELLITE1 servicers under appconfDEVICE_CTRL_SPI:
 * - GPIO servicer
 * - DFU servicer
 * - Audio pipeline tile0 servicer (mic output)
 * - Audio pipeline tile1 servicer (speaker + mic input)
 */
#define APP_DEVICE_CTRL_BASE_SERVICER_COUNT 4

#define APP_DEVICE_CTRL_TOTAL_SERVICER_COUNT \
    (APP_DEVICE_CTRL_BASE_SERVICER_COUNT + \
     appconfAUDIO_CFG_SERVICER_COMPAT_ENABLED + \
     appconfLED_RING + \
     !!(appconfCHAIN_CONTROL_ENABLED) + \
     !!(BUILTIN_TESTS_SPI_ECHO_SERVICER))
