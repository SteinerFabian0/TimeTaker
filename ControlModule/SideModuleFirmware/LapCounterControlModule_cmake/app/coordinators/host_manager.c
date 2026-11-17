/*
 * host_manager.c
 *
 * @brief  The control module's side of the host protocol. See host_manager.h.
 */

#include "host_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "ctrl_fw_version.h"
#include "host_protocol.h"
#include "host_session.h"
#include "event_log.h"
#include "usb_cdc.h"
#include "sys_time.h"
#include "device_id.h"
#include "lane_manager.h"

/* A REBOOT answer has to reach the host before the reset cuts USB. */
#define HOST_MANAGER_REBOOT_DELAY_MS  100U

static eventLogSlot_t hostManagerLogSlots[HOST_MANAGER_EVENT_LOG_CAPACITY];
static eventLog_t     hostManagerLog;

static hostSession_t  hostManagerUsb;

static bool     hostManagerRebootPending;
static uint32_t hostManagerRebootAtMs;


/* --- Transports ---------------------------------------------------------- */

static const hostTransport_t hostManagerUsbIo = {
    .read       = usbCdcRead,
    .writeSpace = usbCdcWriteSpace,
    .write      = usbCdcWrite,
    .flush      = usbCdcFlush,
};

static uint8_t hostManagerOtherSessions(const hostSession_t *self)
{
    (void)self;
    return 0U;   /* one transport so far */
}


/* --- Requests past the session layer -------------------------------------- */

static ctrlStatus_e hostManagerNodeList(uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen)
{
    const uint16_t len = (uint16_t)(HP_NODE_LIST_RSP_BYTES + HP_NODE_LIST_RSP_ITEM_BYTES);
    if (rspCap < len) {
        return CTRL_ERR_INVALID_STATE;
    }

    /* The control module itself. Lanes join here in step 3, the BLE bridge
     * once its CTRL channel reports a version. */
    const hpNodeInfo_t ctrl = {
        .nodeClass  = (uint8_t)HP_NODE_CTRL,
        .index      = 0U,
        .boardType  = (uint8_t)HP_BOARD_CONTROL,
        .state      = (uint8_t)HP_NODE_STATE_OK,
        .fwMajor    = (uint8_t)CTRL_FW_VERSION_MAJOR,
        .fwMinor    = (uint8_t)CTRL_FW_VERSION_MINOR,
        .fwPatch    = (uint8_t)CTRL_FW_VERSION_PATCH,
        .protoMinor = (uint8_t)HP_PROTO_MINOR,
        .fwGitRev   = (uint32_t)CTRL_FW_GIT_REV,
        .imageCrc   = 0U,   /* the bootloader's image header will carry it */
    };
    memcpy(rsp + HP_NODE_LIST_RSP_BYTES, &ctrl, sizeof(ctrl));
    *rspLen = len;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerLanePower(const uint8_t *req, uint16_t reqLen,
                                         uint8_t *rsp, uint16_t *rspLen)
{
    if (reqLen != HP_LANE_POWER_REQ_BYTES) {
        return CTRL_ERR_BAD_LENGTH;
    }
    hpLanePowerReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.on > 1U) {
        return CTRL_ERR_INVALID_ARG;
    }

    CTRL_RETURN_IF_ERR(laneManagerSetPower(r.on != 0U));

    hpLanePowerRsp_t *out = (hpLanePowerRsp_t *)rsp;
    out->powered = laneManagerIsPowered() ? 1U : 0U;
    *rspLen = HP_LANE_POWER_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerReboot(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    if (reqLen != HP_REBOOT_REQ_BYTES) {
        return CTRL_ERR_BAD_LENGTH;
    }
    hpRebootReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.target == (uint8_t)HP_REBOOT_BOOTLOADER) {
        return CTRL_ERR_UNSUPPORTED;   /* no bootloader yet (build order step 7) */
    }
    if (r.target != (uint8_t)HP_REBOOT_APP) {
        return CTRL_ERR_INVALID_ARG;
    }

    hostManagerRebootPending = true;
    hostManagerRebootAtMs    = HAL_GetTick() + HOST_MANAGER_REBOOT_DELAY_MS;
    *rspLen = HP_REBOOT_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerOnRequest(hostSession_t *session, uint8_t msgId,
                                         const uint8_t *req, uint16_t reqLen,
                                         uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen)
{
    (void)session;

    switch (msgId) {
    case HP_MSG_NODE_LIST:
        if (reqLen != HP_NODE_LIST_REQ_BYTES) {
            return CTRL_ERR_BAD_LENGTH;
        }
        return hostManagerNodeList(rsp, rspCap, rspLen);

    case HP_MSG_LANE_POWER:
        return hostManagerLanePower(req, reqLen, rsp, rspLen);

    case HP_MSG_REBOOT:
        return hostManagerReboot(req, reqLen, rspLen);

    case HP_MSG_BIND_ALLOW:
        return CTRL_ERR_UNSUPPORTED;   /* no BLE bridge yet (build order step 6) */

    default:
        return CTRL_ERR_UNSUPPORTED;
    }
}


/* --- API --------------------------------------------------------------------- */

ctrlStatus_e hostManagerPublishEvent(uint8_t msgId, const void *event, uint16_t size)
{
    return eventLogAppend(&hostManagerLog, msgId, event, size, NULL);
}

ctrlStatus_e hostManagerInit(void)
{
    eventLogInit(&hostManagerLog, hostManagerLogSlots, HOST_MANAGER_EVENT_LOG_CAPACITY);
    hostManagerRebootPending = false;

    const hostSessionConfig_t usbCfg = {
        .transport     = (uint8_t)HP_TRANSPORT_USB,
        .bindState     = (uint8_t)HP_BIND_TRUSTED,   /* a cable is physical access */
        .bootId        = deviceIdBootId(),
        .log           = &hostManagerLog,
        .nowUs         = sysTimeNowUs,
        .otherSessions = hostManagerOtherSessions,
        .onRequest     = hostManagerOnRequest,
    };
    hostSessionInit(&hostManagerUsb, &hostManagerUsbIo, &usbCfg);

    const hpEvtBoot_t boot = {
        .resetCause = deviceIdResetCause(),
        .bootId     = deviceIdBootId(),
        .sysUs      = sysTimeNowUs(),
    };
    return hostManagerPublishEvent(HP_MSG_EVT_BOOT, &boot, sizeof(boot));
}

ctrlStatus_e hostManagerRun(void)
{
    if (usbCdcTakeLinkReset()) {
        hostSessionReset(&hostManagerUsb);
    }
    hostSessionRun(&hostManagerUsb);

    if (hostManagerRebootPending && (int32_t)(HAL_GetTick() - hostManagerRebootAtMs) >= 0) {
        NVIC_SystemReset();
    }
    return CTRL_OK;
}

bool hostManagerAnySessionOpen(void)
{
    return hostSessionIsOpen(&hostManagerUsb);
}

bool hostManagerAnyLinkUp(void)
{
    return usbCdcIsMounted();
}

uint32_t hostManagerTrafficCount(void)
{
    return hostManagerUsb.stats.framesIn + hostManagerUsb.stats.eventsSent;
}
