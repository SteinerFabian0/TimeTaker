/*
 * Settings store. See settings_store.h.
 */

#include "settings_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "flash_drv.h"
#include "settings_record.h"
#include "lane_logic.h"

#define SETTINGS_STORE_QW_PER_PASS  8U
#define SETTINGS_STORE_RECORD_MAX   SETTINGS_RECORD_BYTES(sizeof(settingsData_t))

_Static_assert(sizeof(settingsData_t) <= 0xFFFFU, "settings payload");
/* boot/boot_main.c reads the bind list at this offset (BOOT_BINDS_OFFSET). */
_Static_assert(offsetof(settingsData_t, binds) == 252U, "bind list offset shared with ctrl_boot");
_Static_assert(SETTINGS_STORE_RECORD_MAX <= SETTINGS_STORE_PAGE_BYTES, "settings record fits a page");

typedef enum {
    SETTINGS_IDLE = 0,
    SETTINGS_ERASE_TARGET,
    SETTINGS_PROGRAM,
    SETTINGS_ERASE_OLD,
} settingsState_e;

static settingsData_t  settingsData;
static bool            settingsDirty;
static uint16_t        settingsWrites;
static settingsState_e settingsState;
static uint8_t         settingsActive;      /* page of the current record (0/1), or 0xFF */
static uint32_t        settingsSequence;
static uint32_t        settingsLastWriteMs;
static bool            settingsEverWritten;

static uint8_t         settingsRecord[SETTINGS_STORE_RECORD_MAX];
static uint32_t        settingsRecordLen;
static uint32_t        settingsProgramPos;
static uint8_t         settingsTarget;

static uint32_t settingsPageAddr(uint8_t page)
{
    return (page == 0U) ? SETTINGS_STORE_PAGE0 : SETTINGS_STORE_PAGE1;
}

static void settingsDefaults(settingsData_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->device.deviceName, "TimeTaker", 9);
    d->device.bleEnabled = 1U;
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        laneLogicDefaultConfig(n, &d->laneConfig[n - 1U]);
        d->laneSampling[n - 1U] = (uint8_t)HP_SAMPLING_OFF;   /* the host switches sensors on */
    }
    bindList_t empty;
    bindListInit(&empty);
    (void)bindListSerialize(&empty, d->binds, sizeof(d->binds));
}

ctrlStatus_e settingsStoreInit(void)
{
    settingsDefaults(&settingsData);
    settingsDirty  = false;
    settingsState  = SETTINGS_IDLE;
    settingsActive = 0xFFU;

    settingsSelect_t sel;
    settingsRecordSelect((const uint8_t *)SETTINGS_STORE_PAGE0, (const uint8_t *)SETTINGS_STORE_PAGE1,
                         SETTINGS_STORE_PAGE_BYTES, &sel);
    settingsTarget = sel.writeTarget;
    if (sel.active < 0) {
        settingsSequence = 0U;
        return sel.corrupt ? CTRL_ERR_SETTINGS_CORRUPT : CTRL_OK;
    }

    settingsRecordHeader_t hdr;
    const uint8_t         *payload = NULL;
    (void)settingsRecordCheck((const uint8_t *)settingsPageAddr((uint8_t)sel.active), SETTINGS_STORE_PAGE_BYTES,
                              &hdr, &payload);
    /* A shorter record (an older layout) keeps the defaults for the rest;
     * a longer one (a newer layout) is cut. Fields only ever get appended. */
    const uint16_t n = (hdr.length < sizeof(settingsData)) ? hdr.length : (uint16_t)sizeof(settingsData);
    memcpy(&settingsData, payload, n);
    settingsActive   = (uint8_t)sel.active;
    settingsSequence = sel.sequence;

    /* A bind list that does not parse is dropped, not the whole record. */
    bindList_t check;
    if (!bindListDeserialize(&check, settingsData.binds, sizeof(settingsData.binds))) {
        bindListInit(&check);
        (void)bindListSerialize(&check, settingsData.binds, sizeof(settingsData.binds));
    }
    return CTRL_OK;
}

const settingsData_t *settingsStoreData(void)
{
    return &settingsData;
}

void settingsStoreSetDevice(const hpDeviceSettings_t *device)
{
    if (memcmp(&settingsData.device, device, sizeof(*device)) != 0) {
        settingsData.device = *device;
        settingsDirty = true;
    }
}

void settingsStoreSetLaneTable(const hpLaneConfig_t config[LANE_MAX_LANES],
                               const uint8_t sampling[LANE_MAX_LANES])
{
    if (memcmp(settingsData.laneConfig, config, sizeof(settingsData.laneConfig)) != 0
        || memcmp(settingsData.laneSampling, sampling, sizeof(settingsData.laneSampling)) != 0) {
        memcpy(settingsData.laneConfig, config, sizeof(settingsData.laneConfig));
        memcpy(settingsData.laneSampling, sampling, sizeof(settingsData.laneSampling));
        settingsDirty = true;
    }
}

void settingsStoreSetBinds(const bindList_t *list)
{
    uint8_t buf[BIND_LIST_SERIAL_BYTES];
    (void)bindListSerialize(list, buf, sizeof(buf));
    if (memcmp(settingsData.binds, buf, sizeof(buf)) != 0) {
        memcpy(settingsData.binds, buf, sizeof(buf));
        settingsDirty = true;
    }
}

static bool settingsPageErased(uint8_t page)
{
    const uint32_t *p = (const uint32_t *)settingsPageAddr(page);
    for (uint32_t i = 0U; i < SETTINGS_STORE_PAGE_BYTES / 4U; i++) {
        if (p[i] != 0xFFFFFFFFU) {
            return false;
        }
    }
    return true;
}

static ctrlStatus_e settingsFail(ctrlStatus_e status)
{
    settingsState       = SETTINGS_IDLE;
    settingsLastWriteMs = HAL_GetTick();   /* retry after the interval, dirty stays */
    settingsEverWritten = true;
    return status;
}

ctrlStatus_e settingsStoreRun(bool allowed)
{
    switch (settingsState) {
    case SETTINGS_IDLE:
        if (!settingsDirty || !allowed
            || (settingsEverWritten && HAL_GetTick() - settingsLastWriteMs < SETTINGS_STORE_MIN_INTERVAL_MS)) {
            return CTRL_OK;
        }
        settingsRecordLen = settingsRecordEncode((const uint8_t *)&settingsData, (uint16_t)sizeof(settingsData),
                                                 settingsSequence + 1U, settingsRecord, sizeof(settingsRecord));
        if (settingsRecordLen == 0U) {
            return settingsFail(CTRL_ERR_INVALID_STATE);
        }
        settingsDirty      = false;   /* a change from here on writes again later */
        settingsProgramPos = 0U;
        if (settingsPageErased(settingsTarget)) {
            settingsState = SETTINGS_PROGRAM;
        } else {
            if (flashDrvEraseStart(settingsPageAddr(settingsTarget)) != CTRL_OK) {
                settingsDirty = true;
                return settingsFail(CTRL_ERR_FLASH);
            }
            settingsState = SETTINGS_ERASE_TARGET;
        }
        return CTRL_OK;

    case SETTINGS_ERASE_TARGET:
        if (flashDrvBusy()) {
            return CTRL_OK;
        }
        if (flashDrvResult() != CTRL_OK) {
            settingsDirty = true;
            return settingsFail(CTRL_ERR_FLASH);
        }
        settingsState = SETTINGS_PROGRAM;
        return CTRL_OK;

    case SETTINGS_PROGRAM:
        if (!allowed) {
            return CTRL_OK;   /* a start sequence began: pause between quad-words, power loss is safe */
        }
        for (uint8_t i = 0U; i < SETTINGS_STORE_QW_PER_PASS && settingsProgramPos < settingsRecordLen; i++) {
            ctrlStatus_e st = flashDrvProgram16(settingsPageAddr(settingsTarget) + settingsProgramPos,
                                                &settingsRecord[settingsProgramPos]);
            if (st != CTRL_OK) {
                settingsDirty = true;
                return settingsFail(CTRL_ERR_FLASH);
            }
            settingsProgramPos += FLASH_DRV_QW_BYTES;
        }
        if (settingsProgramPos < settingsRecordLen) {
            return CTRL_OK;
        }
        if (settingsRecordCheck((const uint8_t *)settingsPageAddr(settingsTarget), SETTINGS_STORE_PAGE_BYTES,
                                NULL, NULL) != SETTINGS_PAGE_VALID) {
            settingsDirty = true;
            return settingsFail(CTRL_ERR_FLASH);
        }
        settingsSequence++;
        settingsWrites++;
        /* The old page: a failed erase start only means the next write
         * erases it first (it is the next target). */
        settingsState = (settingsActive <= 1U && flashDrvEraseStart(settingsPageAddr(settingsActive)) == CTRL_OK)
                        ? SETTINGS_ERASE_OLD : SETTINGS_IDLE;
        settingsActive      = settingsTarget;
        settingsTarget      = (uint8_t)(settingsTarget ^ 1U);
        settingsLastWriteMs = HAL_GetTick();
        settingsEverWritten = true;
        return CTRL_OK;

    case SETTINGS_ERASE_OLD:
        if (flashDrvBusy()) {
            return CTRL_OK;
        }
        settingsState = SETTINGS_IDLE;
        /* A failed erase only means the next write erases it first. */
        return (flashDrvResult() == CTRL_OK) ? CTRL_OK : CTRL_ERR_FLASH;

    default:
        settingsState = SETTINGS_IDLE;
        return CTRL_OK;
    }
}

bool settingsStoreIsDirty(void)
{
    return settingsDirty || settingsState != SETTINGS_IDLE;
}

uint16_t settingsStoreWrites(void)
{
    return settingsWrites;
}
