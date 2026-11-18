/*
 * nRF link protocol, pure part. See nrf_link_proto.h.
 */

#include "nrf_link_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Body size each nRF -> STM CTRL type needs; 0 = not a type the STM accepts. */
static uint16_t nrfLinkProtoCtrlSize(uint8_t type)
{
    switch (type) {
    case BLE_CTRL_NRF_HELLO: return (uint16_t)sizeof(bleCtrlNrfHello_t);
    case BLE_CTRL_LINK_UP:   return (uint16_t)sizeof(bleCtrlLinkUp_t);
    case BLE_CTRL_LINK_DOWN: return (uint16_t)sizeof(bleCtrlLinkDown_t);
    case BLE_CTRL_BUTTON:    return (uint16_t)sizeof(bleCtrlButton_t);
    case BLE_CTRL_CREDIT:    return (uint16_t)sizeof(bleCtrlCredit_t);
    case BLE_CTRL_STATS:     return (uint16_t)sizeof(bleCtrlStats_t);
    default:                 return 0U;
    }
}

nrfLinkParse_e nrfLinkProtoParse(const uint8_t *raw, uint16_t len, nrfLinkMsg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    if (len < 1U) {
        return NRF_LINK_PARSE_BAD_CHANNEL;
    }

    if (raw[0] == (uint8_t)BLE_LINK_CH_DATA) {
        const uint16_t n = (uint16_t)(len - 1U);
        if (n == 0U || n > BLE_LINK_DATA_MAX) {
            return NRF_LINK_PARSE_BAD_DATA;
        }
        msg->data    = &raw[1];
        msg->dataLen = n;
        return NRF_LINK_PARSE_DATA;
    }

    if (raw[0] != (uint8_t)BLE_LINK_CH_CTRL) {
        return NRF_LINK_PARSE_BAD_CHANNEL;
    }
    if (len < 2U) {
        return NRF_LINK_PARSE_SHORT_CTRL;
    }

    const uint8_t  type = raw[1];
    const uint16_t need = nrfLinkProtoCtrlSize(type);
    const uint16_t have = (uint16_t)(len - 2U);
    msg->ctrlType = type;
    if (need == 0U) {
        return NRF_LINK_PARSE_UNKNOWN_CTRL;
    }
    if (have < need) {
        return NRF_LINK_PARSE_SHORT_CTRL;
    }
    memcpy(&msg->ctrl, &raw[2], need);   /* extra bytes: a newer MINOR, ignored */
    return NRF_LINK_PARSE_CTRL;
}

bool nrfLinkProtoVersionOk(const bleCtrlNrfHello_t *hello)
{
    return hello->linkMajor == BLE_LINK_MAJOR;
}

void nrfLinkCreditsReset(nrfLinkCredits_t *c, uint8_t granted)
{
    c->credits = granted;
    c->granted = granted;
}

void nrfLinkCreditsReturn(nrfLinkCredits_t *c, uint8_t count)
{
    /* NRF_HELLO.credits is the free slots at that moment; busy slots return
     * theirs later, so the count may legitimately climb above `granted` up to
     * the nRF's slot total, which the STM does not know. Only a uint8_t
     * overflow is a desync. */
    uint16_t sum = (uint16_t)c->credits + count;
    if (sum > 255U) {
        c->overReturned++;
        sum = 255U;
    }
    c->credits = (uint8_t)sum;
}

bool nrfLinkCreditsTake(nrfLinkCredits_t *c)
{
    if (c->credits == 0U) {
        return false;
    }
    c->credits--;
    return true;
}

uint16_t nrfLinkProtoEncodeData(const uint8_t *data, uint16_t len, uint8_t *out, uint16_t cap)
{
    if (len == 0U || len > BLE_LINK_DATA_MAX || data == NULL) {
        return 0U;
    }
    const uint8_t ch = (uint8_t)BLE_LINK_CH_DATA;
    return frameEncode(&ch, 1U, data, len, out, cap);
}

uint16_t nrfLinkProtoEncodeCtrl(uint8_t type, const void *body, uint16_t bodyLen,
                                uint8_t *out, uint16_t cap)
{
    if (bodyLen > BLE_LINK_DATA_MAX - 1U || (body == NULL && bodyLen != 0U)) {
        return 0U;
    }
    const uint8_t head[2] = { (uint8_t)BLE_LINK_CH_CTRL, type };
    return frameEncode(head, 2U, (const uint8_t *)body, bodyLen, out, cap);
}
