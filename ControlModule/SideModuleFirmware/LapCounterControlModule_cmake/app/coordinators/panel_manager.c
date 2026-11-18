/*
 * panel_manager.c
 *
 * @brief  Panel LEDs and buttons. See panel_manager.h.
 */

#include "panel_manager.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "host_protocol.h"
#include "indicator_led.h"
#include "button_handler.h"
#include "sys_time.h"
#include "host_manager.h"
#include "sequence_manager.h"
#include "ctrl_behavior.h"
#include "fault_guard.h"
#include "ctrl_policy.h"

typedef ctrlPolicyLink_e panelLink_e;   /* ctrl_policy.h */
#define PANEL_LINK_NO_HOST     CTRL_POLICY_LINK_NO_HOST
#define PANEL_LINK_NO_SESSION  CTRL_POLICY_LINK_NO_SESSION
#define PANEL_LINK_SESSION     CTRL_POLICY_LINK_SESSION
#define PANEL_LINK_UNKNOWN     CTRL_POLICY_LINK_UNKNOWN

static panelLink_e panelLinkShown;
static uint32_t    panelTrafficSeen;
static uint32_t    panelLedsFreeAtMs;

static ctrlStatus_e panelShowLink(panelLink_e link)
{
    switch (link) {
    case PANEL_LINK_NO_HOST:
        return indicatorLedBlink(INDICATOR_LED_STATUS, 2000U, 50U, 0U);
    case PANEL_LINK_NO_SESSION:
        return indicatorLedBlink(INDICATOR_LED_STATUS, 1000U, 500U, 0U);
    case PANEL_LINK_SESSION:
        return indicatorLedSet(INDICATOR_LED_STATUS, true);
    default:
        return indicatorLedSet(INDICATOR_LED_STATUS, false);
    }
}

static panelLink_e panelCurrentLink(void)
{
    return ctrlPolicyPanelLink(hostManagerAnySessionOpen(), hostManagerAnyLinkUp());
}

static ctrlStatus_e panelPublishButtons(void)
{
    ctrlStatus_e result = CTRL_OK;

    for (int id = (int)BUTTON_BTN1; id <= (int)BUTTON_COUNT; id++) {
        buttonPress_e press = buttonHandlerTakePress((buttonId_e)id);
        if (press == BUTTON_PRESS_NONE) {
            continue;
        }
        /* buttonId_e/buttonPress_e values equal hpButtonId_e/hpPressKind_e. */
        const hpEvtButton_t evt = {
            .button = (uint8_t)id,
            .press  = (uint8_t)press,
            .sysUs  = sysTimeNowUs(),
        };
        ctrlStatus_e status = hostManagerPublishEvent(HP_MSG_EVT_BUTTON, &evt, sizeof(evt));
        if (status != CTRL_OK) {
            result = status;
        }

        /* Local fallback: abort the start without a host. */
        if (ctrlPolicyButtonAborts((uint8_t)id, (uint8_t)press, sequenceManagerIsRunning())) {
            status = sequenceManagerAbort(HP_ABORT_BUTTON);
            if (status != CTRL_OK) {
                result = status;
            }
        }
#if CTRL_ENABLE_FAULT_INJECT
        if (press == BUTTON_PRESS_VERY_LONG && id == (int)BUTTON_BTN1) {
            faultGuardInjectHardFault();
        }
        if (press == BUTTON_PRESS_VERY_LONG && id == (int)BUTTON_BTN3) {
            faultGuardInjectHang();
        }
#endif
    }
    return result;
}

ctrlStatus_e panelManagerInit(uint32_t ledsFreeAtMs)
{
    panelLinkShown    = PANEL_LINK_UNKNOWN;
    panelTrafficSeen  = hostManagerTrafficCount();
    panelLedsFreeAtMs = ledsFreeAtMs;
    return CTRL_OK;
}

ctrlStatus_e panelManagerRun(void)
{
    ctrlStatus_e result = panelPublishButtons();

    if ((int32_t)(HAL_GetTick() - panelLedsFreeAtMs) < 0) {
        return result;                   /* boot flash still playing */
    }

    panelLink_e link = panelCurrentLink();
    if (link != panelLinkShown) {
        ctrlStatus_e status = panelShowLink(link);
        if (status == CTRL_OK) {
            panelLinkShown = link;       /* else retried next pass */
        } else {
            result = status;
        }
    }

    uint32_t traffic = hostManagerTrafficCount();
    if (traffic != panelTrafficSeen && !indicatorLedIsBlinking(INDICATOR_LED_ACT)) {
        ctrlStatus_e status = indicatorLedBlink(INDICATOR_LED_ACT, 60U, 500U, 1U);
        if (status == CTRL_OK) {
            panelTrafficSeen = traffic;  /* else retried next pass */
        } else {
            result = status;
        }
    }
    return result;
}
