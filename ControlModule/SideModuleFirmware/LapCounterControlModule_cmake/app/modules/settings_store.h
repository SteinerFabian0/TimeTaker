/*
 * Settings store: what survives a power cycle (CONTROL_FIRMWARE_GUIDE §5.5).
 *
 * Contents (settingsData_t): the device settings (name, BLE enabled), the
 * lane config table and sampling modes, and the BLE bind list. Owners keep
 * their working copy and hand changes in with the setters; the store only
 * remembers and writes.
 *
 * Where: the last two 8 KB pages of the flash, 0x0803_C000 and 0x0803_E000
 * (bank 2, pages 14 and 15; flash_drv.h has the whole map). Bank 2 is
 * read-while-write against bank 1 where the code runs, so an erase there does
 * not stall the loop, USB or the SYNC timer. One record per page, used in
 * turn (settings_record.h): the new record goes into the erased page, then
 * the old page is erased.
 *
 * When: never on demand. A setter marks the data dirty; settingsStoreRun()
 * writes when it is allowed (ctrl_master passes "no start sequence runs"),
 * at most once every SETTINGS_STORE_MIN_INTERVAL_MS, as a state machine of a
 * few quad-words per pass. A write already under way finishes.
 *
 * At boot: the newer valid record wins. None valid: compiled defaults; if a
 * page held garbage, settingsStoreInit() returns CTRL_ERR_SETTINGS_CORRUPT
 * once (ctrl_master records it as the last error and logs it).
 *
 * Owns: the two settings pages (through flash_drv). Context: main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"
#include "host_protocol.h"
#include "lane_protocol.h"
#include "bind_list.h"

#define SETTINGS_STORE_PAGE0            0x0803C000U
#define SETTINGS_STORE_PAGE1            0x0803E000U
#define SETTINGS_STORE_PAGE_BYTES       0x2000U
_Static_assert(SETTINGS_STORE_PAGE0 == 0x0803C000U && SETTINGS_STORE_PAGE1 + SETTINGS_STORE_PAGE_BYTES == 0x08040000U,
               "settings pages: keep in step with FLASH_DRV_SETTINGS_BASE (flash_drv.h) and the linker script");
#define SETTINGS_STORE_MIN_INTERVAL_MS  10000U

typedef struct __attribute__((packed)) {
    hpDeviceSettings_t device;
    hpLaneConfig_t     laneConfig[LANE_MAX_LANES];
    uint8_t            laneSampling[LANE_MAX_LANES];   /* hpSamplingMode_e per lane */
    uint8_t            reserved[2];
    uint8_t            binds[BIND_LIST_SERIAL_BYTES];   /* bindListSerialize() */
} settingsData_t;

/* Loads the current record or the defaults. CTRL_ERR_SETTINGS_CORRUPT if
 * stored data was found but none of it was valid. */
ctrlStatus_e settingsStoreInit(void);

const settingsData_t *settingsStoreData(void);

void settingsStoreSetDevice(const hpDeviceSettings_t *device);
void settingsStoreSetLaneTable(const hpLaneConfig_t config[LANE_MAX_LANES],
                               const uint8_t sampling[LANE_MAX_LANES]);
void settingsStoreSetBinds(const bindList_t *list);

/* Every pass. allowed = false holds back a new write (a start sequence runs). */
ctrlStatus_e settingsStoreRun(bool allowed);

bool     settingsStoreIsDirty(void);
uint16_t settingsStoreWrites(void);
