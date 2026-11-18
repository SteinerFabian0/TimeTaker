/*
 * The BLE side. See ble_link.h.
 */

#include "ble_link.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <bluetooth/services/nus.h>

LOG_MODULE_REGISTER(ble_link, LOG_LEVEL_INF);

/* Advertising (guide §6.1): flags + the 128-bit NUS UUID in the advertising
 * data, the complete name in the scan response (both do not fit in 31 B).
 * Connectable, undirected, legacy, 100 ms. */
#define ADV_INTERVAL_UNITS 160U                     /* 0.625 ms units: 100 ms */

static const struct bt_data advData[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static const struct bt_data scanRsp[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void (*wakeFn)(void);
static bleWriteHandler_t writeFn;

static atomic_t events;
static atomic_t connectedFlag;
static atomic_t notifyFlag;
static atomic_t advertisingFlag;
static atomic_t lastReason;

/* The connection, with our reference. Guarded by connLock. */
static struct k_spinlock connLock;
static struct bt_conn   *currentConn;

/* Notification completions per slot, and the connection generation they
 * belong to. Guarded by sentLock. */
static struct k_spinlock sentLock;
static uint32_t          generation;
static uint8_t           sentCount[SLOTS_MAX];

static const struct bt_gatt_attr *nusTxAttr;

static void postEvent(uint32_t ev)
{
    atomic_or(&events, (atomic_val_t)ev);
    if (wakeFn != NULL) {
        wakeFn();
    }
}

/* A reference to the current connection, or NULL. Unref when done. */
static struct bt_conn *connGet(void)
{
    k_spinlock_key_t key = k_spin_lock(&connLock);
    struct bt_conn *c = (currentConn != NULL) ? bt_conn_ref(currentConn) : NULL;
    k_spin_unlock(&connLock, key);
    return c;
}

static void newGeneration(void)
{
    k_spinlock_key_t key = k_spin_lock(&sentLock);
    generation = (generation + 1U) & 0x00FFFFFFU;
    for (uint8_t i = 0U; i < SLOTS_MAX; i++) {
        sentCount[i] = 0U;
    }
    k_spin_unlock(&sentLock, key);
}

/* --- Connection callbacks (BT RX thread) ---------------------------------------- */

static void onConnected(struct bt_conn *conn, uint8_t err)
{
    atomic_set(&advertisingFlag, 0);
    if (err != 0U) {
        LOG_WRN("connection failed: 0x%02x", err);
        postEvent(BLE_EVT_RECYCLED);           /* advertise again */
        return;
    }

    newGeneration();
    k_spinlock_key_t key = k_spin_lock(&connLock);
    if (currentConn != NULL) {
        bt_conn_unref(currentConn);
    }
    currentConn = bt_conn_ref(conn);
    k_spin_unlock(&connLock, key);

    atomic_set(&notifyFlag, 0);
    atomic_set(&connectedFlag, 1);
    postEvent(BLE_EVT_CONNECTED);
}

static void onDisconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);

    newGeneration();
    k_spinlock_key_t key = k_spin_lock(&connLock);
    struct bt_conn *old = currentConn;
    currentConn = NULL;
    k_spin_unlock(&connLock, key);
    if (old != NULL) {
        bt_conn_unref(old);
    }

    atomic_set(&lastReason, reason);
    atomic_set(&notifyFlag, 0);
    atomic_set(&connectedFlag, 0);
    postEvent(BLE_EVT_DISCONNECTED);
}

static void onRecycled(void)
{
    postEvent(BLE_EVT_RECYCLED);
}

static void onParamUpdated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(interval);
    ARG_UNUSED(latency);
    ARG_UNUSED(timeout);
    postEvent(BLE_EVT_PARAMS);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void onPhyUpdated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(param);
    postEvent(BLE_EVT_PARAMS);
}
#endif

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
static void onDataLenUpdated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(info);
    postEvent(BLE_EVT_PARAMS);
}
#endif

BT_CONN_CB_DEFINE(connCallbacks) = {
    .connected        = onConnected,
    .disconnected     = onDisconnected,
    .recycled         = onRecycled,
    .le_param_updated = onParamUpdated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
    .le_phy_updated   = onPhyUpdated,
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
    .le_data_len_updated = onDataLenUpdated,
#endif
};

static void onMtuUpdated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(tx);
    ARG_UNUSED(rx);
    postEvent(BLE_EVT_PARAMS);
}

static struct bt_gatt_cb gattCallbacks = {
    .att_mtu_updated = onMtuUpdated,
};

/* --- NUS ------------------------------------------------------------------------- */

static void onNusReceived(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
    ARG_UNUSED(conn);
    if (writeFn != NULL) {
        writeFn(data, len);
    }
}

static void onNusSendEnabled(enum bt_nus_send_status status)
{
    const bool on = (status == BT_NUS_SEND_STATUS_ENABLED);
    atomic_set(&notifyFlag, on ? 1 : 0);
    postEvent(on ? BLE_EVT_NOTIFY_ON : BLE_EVT_NOTIFY_OFF);
}

static struct bt_nus_cb nusCallbacks = {
    .received     = onNusReceived,
    .send_enabled = onNusSendEnabled,
};

/* Completion of one notification (system work queue). */
static void onNotifySent(struct bt_conn *conn, void *user_data)
{
    ARG_UNUSED(conn);
    const uint32_t tag  = (uint32_t)(uintptr_t)user_data;
    const uint8_t  slot = (uint8_t)(tag & 0xFFU);

    k_spinlock_key_t key = k_spin_lock(&sentLock);
    if ((tag >> 8) == generation && slot < SLOTS_MAX && sentCount[slot] < UINT8_MAX) {
        sentCount[slot]++;
    }
    k_spin_unlock(&sentLock, key);
    postEvent(BLE_EVT_SENT);
}

/* --- API ------------------------------------------------------------------------- */

int bleLinkInit(void (*wake)(void), bleWriteHandler_t onWrite)
{
    wakeFn  = wake;
    writeFn = onWrite;

    int err = bt_enable(NULL);
    if (err != 0) {
        return err;
    }
    bt_gatt_cb_register(&gattCallbacks);

    err = bt_nus_init(&nusCallbacks);
    if (err != 0) {
        return err;
    }
    nusTxAttr = bt_gatt_find_by_uuid(NULL, 0, BT_UUID_NUS_TX);
    return (nusTxAttr != NULL) ? 0 : -ENOENT;
}

uint32_t bleLinkTakeEvents(void)
{
    return (uint32_t)atomic_clear(&events);
}

uint8_t bleLinkLastDisconnectReason(void)
{
    return (uint8_t)atomic_get(&lastReason);
}

bool bleLinkConnected(void)
{
    return atomic_get(&connectedFlag) != 0;
}

bool bleLinkNotifyEnabled(void)
{
    return atomic_get(&notifyFlag) != 0;
}

int bleLinkAdvertiseStart(void)
{
    if (bleLinkConnected() || bleLinkAdvertising()) {
        return 0;
    }
    const struct bt_le_adv_param *param =
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADV_INTERVAL_UNITS, ADV_INTERVAL_UNITS, NULL);
    int err = bt_le_adv_start(param, advData, ARRAY_SIZE(advData), scanRsp, ARRAY_SIZE(scanRsp));
    if (err == 0 || err == -EALREADY) {
        atomic_set(&advertisingFlag, 1);
        return 0;
    }
    return err;
}

int bleLinkAdvertiseStop(void)
{
    atomic_set(&advertisingFlag, 0);
    const int err = bt_le_adv_stop();
    return err;
}

bool bleLinkAdvertising(void)
{
    return atomic_get(&advertisingFlag) != 0;
}

int bleLinkKick(uint8_t hciReason)
{
    struct bt_conn *c = connGet();
    if (c == NULL) {
        return -ENOTCONN;
    }
    const int err = bt_conn_disconnect(c, hciReason);
    bt_conn_unref(c);
    return err;
}

uint16_t bleLinkMaxChunk(void)
{
    struct bt_conn *c = connGet();
    if (c == NULL) {
        return 0U;
    }
    const uint32_t mtu = bt_gatt_get_mtu(c);
    bt_conn_unref(c);
    if (mtu <= 3U) {
        return 0U;
    }
    const uint32_t payload = mtu - 3U;
    return (uint16_t)((payload < BLE_LINK_DATA_MAX) ? payload : BLE_LINK_DATA_MAX);
}

uint8_t bleLinkMaxInFlight(void)
{
    return (CONFIG_BT_ATT_TX_COUNT > 1) ? (uint8_t)(CONFIG_BT_ATT_TX_COUNT - 1) : 1U;
}

int bleLinkNotify(const uint8_t *data, uint16_t len, uint8_t slot)
{
    struct bt_conn *c = connGet();
    if (c == NULL) {
        return -ENOTCONN;
    }

    k_spinlock_key_t key = k_spin_lock(&sentLock);
    const uint32_t tag = (generation << 8) | slot;
    k_spin_unlock(&sentLock, key);

    struct bt_gatt_notify_params params = {
        .attr      = nusTxAttr,
        .data      = data,
        .len       = len,
        .func      = onNotifySent,
        .user_data = (void *)(uintptr_t)tag,
    };
    const int err = bt_gatt_notify_cb(c, &params);
    bt_conn_unref(c);
    return err;
}

void bleLinkNewSession(void)
{
    newGeneration();
}

void bleLinkTakeCompletions(uint8_t counts[SLOTS_MAX])
{
    k_spinlock_key_t key = k_spin_lock(&sentLock);
    for (uint8_t i = 0U; i < SLOTS_MAX; i++) {
        counts[i] = (uint8_t)(counts[i] + sentCount[i]);
        sentCount[i] = 0U;
    }
    k_spin_unlock(&sentLock, key);
}

bool bleLinkGetLinkUp(bleCtrlLinkUp_t *out)
{
    struct bt_conn *c = connGet();
    if (c == NULL) {
        return false;
    }

    struct bt_conn_info info;
    const int err = bt_conn_get_info(c, &info);
    const uint16_t mtu = bt_gatt_get_mtu(c);
    bt_conn_unref(c);
    if (err != 0) {
        return false;
    }

    *out = (bleCtrlLinkUp_t){
        .attMtu        = mtu,
        .txPhy         = 1U,
        .rxPhy         = 1U,
        .intervalUnits = (uint16_t)(info.le.interval_us / 1250U),
        .latency       = info.le.latency,
        .timeout10ms   = info.le.timeout,
        .maxTxOctets   = 27U,
    };
#if defined(CONFIG_BT_USER_PHY_UPDATE)
    if (info.le.phy != NULL) {
        out->txPhy = (info.le.phy->tx_phy == BT_GAP_LE_PHY_2M) ? 2U : 1U;
        out->rxPhy = (info.le.phy->rx_phy == BT_GAP_LE_PHY_2M) ? 2U : 1U;
    }
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
    if (info.le.data_len != NULL) {
        out->maxTxOctets = info.le.data_len->tx_max_len;
    }
#endif
    return true;
}

int8_t bleLinkRssi(void)
{
    int8_t rssi = 127;
    struct bt_conn *c = connGet();
    if (c == NULL) {
        return rssi;
    }

    uint16_t handle;
    const int herr = bt_hci_get_conn_handle(c, &handle);
    bt_conn_unref(c);
    if (herr != 0) {
        return rssi;
    }

    struct net_buf *buf = bt_hci_cmd_alloc(K_MSEC(10));
    if (buf == NULL) {
        return rssi;
    }
    struct bt_hci_cp_read_rssi *cp = net_buf_add(buf, sizeof(*cp));
    cp->handle = sys_cpu_to_le16(handle);

    struct net_buf *rsp = NULL;
    if (bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp) == 0 && rsp != NULL) {
        const struct bt_hci_rp_read_rssi *rp = (const void *)rsp->data;
        if (rp->status == 0U) {
            rssi = rp->rssi;
        }
    }
    if (rsp != NULL) {
        net_buf_unref(rsp);
    }
    return rssi;
}

void bleLinkGetAddr(uint8_t out[6])
{
    bt_addr_le_t addrs[1];
    size_t count = ARRAY_SIZE(addrs);

    bt_id_get(addrs, &count);
    if (count == 0U) {
        memset(out, 0, 6);
        return;
    }
    memcpy(out, addrs[0].a.val, 6);
}
