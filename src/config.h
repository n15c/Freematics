// Compile-time constants. Everything a user may want to change at runtime
// lives in settings.h and is editable in the web UI.
#pragma once

#ifndef FW_VERSION
#define FW_VERSION "2.0.0"
#endif

// ---------------------------------------------------------------------------
// Queue (records waiting for upload)
// ---------------------------------------------------------------------------
#ifdef BOARD_HAS_PSRAM
#define QUEUE_RAM_RECORDS 3000
#else
#define QUEUE_RAM_RECORDS 40
#endif
// overflow file on LittleFS used while there is no connectivity
#define QUEUE_FILE "/queue.bin"
#define QUEUE_INDEX_FILE "/queue.idx"
#define QUEUE_FILE_MAX_BYTES (160 * 1024)

// max length of the extra attribute string of one record (OBD/CAN values)
#define RECORD_ATTR_LEN 300

// ---------------------------------------------------------------------------
// Limits for configurable items
// ---------------------------------------------------------------------------
#define MAX_OBD_PIDS 24
#define MAX_CAN_SIGNALS 16
#define CAN_MONITOR_SLOTS 48

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define MAIN_TICK_MS 20
#define VOLTAGE_CHECK_MS 1000
#define OBD_RETRY_MS 4000          // retry OBD init while ignition is on
#define CAN_SNIFF_WINDOW_MS 120    // listening window per sniff cycle
#define GNSS_REINIT_MS 300000UL    // restart GNSS receiver after 5 min without data
#define MODEM_MIN_POWERCYCLE_MS 180000UL // never power cycle the modem more often (operator bans)
#define HTTP_TIMEOUT_MS 15000
#define WIFI_CONNECT_TIMEOUT_MS 8000
#define STANDBY_POLL_MS 200
#define CONFIG_WINDOW_MS 600000UL     // stay awake (AP on) for 10 min after power-up

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
#define WEB_PORT 80
#define MDNS_NAME "freematics"
