/*
 * nRF <-> STM link messages. See link_msg.h.
 */

#include "link_msg.h"

#include <stddef.h>
#include <string.h>

/* The packed structs go on the wire as they are in memory: little-endian. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "link structs are little-endian");

uint16_t linkCtrlSize(uint8_t type)
{
    switch (type) {
    case BLE_CTRL_NRF_HELLO: return (uint16_t)sizeof(bleCtrlNrfHello_t);
    case BLE_CTRL_LINK_UP:   return (uint16_t)sizeof(bleCtrlLinkUp_t);
    case BLE_CTRL_LINK_DOWN: return (uint16_t)sizeof(bleCtrlLinkDown_t);
    case BLE_CTRL_BUTTON:    return (uint16_t)sizeof(bleCtrlButton_t);
    case BLE_CTRL_CREDIT:    return (uint16_t)sizeof(bleCtrlCredit_t);
    case BLE_CTRL_STATS:     return (uint16_t)sizeof(bleCtrlStats_t);
    case BLE_CTRL_STM_HELLO: return (uint16_t)sizeof(bleCtrlStmHello_t);
    case BLE_CTRL_UI_STATE:  return (uint16_t)sizeof(bleCtrlUiState_t);
    case BLE_CTRL_CUE:       return (uint16_t)sizeof(bleCtrlCue_t);
    case BLE_CTRL_KICK:      return (uint16_t)sizeof(bleCtrlKick_t);
    case BLE_CTRL_ADVERTISE: return (uint16_t)sizeof(bleCtrlAdvertise_t);
    default:                 return 0U;
    }
}

static bool isStmToNrf(uint8_t type)
{
    return type >= 0x41U && type <= 0x7FU;
}

static linkMsgResult_e parseCtrl(const uint8_t *body, uint16_t len, linkCtrlMsg_t *out)
{
    if (len < 1U) {
        return LINK_MSG_ERR_CTRL_SHORT;
    }
    const uint8_t  type = body[0];
    const uint16_t need = linkCtrlSize(type);

    if (need == 0U || !isStmToNrf(type)) {
        return LINK_MSG_CTRL_UNKNOWN;
    }
    if ((uint16_t)(len - 1U) < need) {
        return LINK_MSG_ERR_CTRL_SHORT;
    }

    memset(out, 0, sizeof(*out));
    out->type = type;
    /* Extra bytes are ignored. The bound is defence in depth: only STM -> nRF
     * types (all smaller than the union) get here. */
    memcpy(&out->u, &body[1], (need < sizeof(out->u)) ? need : sizeof(out->u));

    switch (type) {
    case BLE_CTRL_UI_STATE:
        if (out->u.uiState.session > BLE_UI_SESSION_BOUND || out->u.uiState.pairWindow > 1U) {
            return LINK_MSG_CTRL_BAD_VALUE;
        }
        break;
    case BLE_CTRL_CUE:
        if (out->u.cue.kind != BLE_CUE_BIND_ACCEPTED && out->u.cue.kind != BLE_CUE_BINDS_CLEARED) {
            return LINK_MSG_CTRL_BAD_VALUE;
        }
        break;
    case BLE_CTRL_ADVERTISE:
        if (out->u.advertise.on > 1U) {
            return LINK_MSG_CTRL_BAD_VALUE;
        }
        break;
    default:
        break;                                    /* STM_HELLO, KICK: any value */
    }
    return LINK_MSG_CTRL;
}

linkMsgResult_e linkParse(const uint8_t *raw, uint16_t rawLen, linkMsg_t *out)
{
    if (rawLen < 1U) {
        return LINK_MSG_ERR_CHANNEL;
    }
    const uint8_t *body    = &raw[1];
    const uint16_t bodyLen = (uint16_t)(rawLen - 1U);

    switch (raw[0]) {
    case BLE_LINK_CH_DATA:
        if (bodyLen == 0U || bodyLen > BLE_LINK_DATA_MAX) {
            return LINK_MSG_ERR_DATA_LEN;
        }
        out->data = body;
        out->len  = bodyLen;
        return LINK_MSG_DATA;
    case BLE_LINK_CH_CTRL:
        return parseCtrl(body, bodyLen, &out->ctrl);
    default:
        return LINK_MSG_ERR_CHANNEL;
    }
}

uint16_t linkEncodeData(const uint8_t *data, uint16_t len, uint8_t *out, uint16_t outCap)
{
    if (len == 0U || len > BLE_LINK_DATA_MAX) {
        return 0U;
    }
    const uint8_t ch = BLE_LINK_CH_DATA;
    return frameEncode(&ch, 1U, data, len, out, outCap);
}

uint16_t linkEncodeCtrl(uint8_t type, const void *msg, uint16_t msgLen, uint8_t *out, uint16_t outCap)
{
    if (msgLen > LINK_CTRL_MSG_MAX || (msg == NULL && msgLen != 0U)) {
        return 0U;
    }
    const uint8_t head[2] = { BLE_LINK_CH_CTRL, type };
    return frameEncode(head, 2U, (const uint8_t *)msg, msgLen, out, outCap);
}

uint8_t linkKickReason(uint8_t stmReason)
{
    switch (stmReason) {
    case 0x05U:   /* authentication failure                      */
    case 0x13U:   /* remote user terminated connection            */
    case 0x14U:   /* remote device terminated: low resources      */
    case 0x15U:   /* remote device terminated: power off          */
    case 0x1AU:   /* unsupported remote feature                   */
    case 0x29U:   /* pairing with unit key not supported          */
    case 0x3BU:   /* unacceptable connection parameters           */
        return stmReason;
    default:
        return LINK_KICK_DEFAULT_REASON;
    }
}

bleResetCause_e linkResetCause(uint32_t resetreas)
{
    /* Several bits can be set if RESETREAS was not cleared; the most telling
     * one wins. */
    if ((resetreas & NRF_RESETREAS_DOG) != 0U) {
        return BLE_RESET_WATCHDOG;
    }
    if ((resetreas & NRF_RESETREAS_LOCKUP) != 0U) {
        return BLE_RESET_LOCKUP;
    }
    if ((resetreas & NRF_RESETREAS_SREQ) != 0U) {
        return BLE_RESET_SOFTWARE;
    }
    if ((resetreas & NRF_RESETREAS_RESETPIN) != 0U) {
        return BLE_RESET_PIN;
    }
    if (resetreas == 0U) {
        return BLE_RESET_POWER_ON;              /* power-on / brown-out: no bit */
    }
    return BLE_RESET_UNKNOWN;                   /* OFF, DIF, LPCOMP, ... */
}

uint16_t linkSat16(uint32_t v)
{
    return (v > 0xFFFFU) ? 0xFFFFU : (uint16_t)v;
}
