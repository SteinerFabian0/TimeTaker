/*
 * lane_protocol.h — the lane bus contract between the lane modules and the
 * control module.
 *
 * THIS FILE IS BYTE-IDENTICAL IN TWO TREES:
 *   LaneModule/LaneModuleFirmware/LapCounterLaneModule_cmake/app/config/lane_protocol.h
 *   ControlModule/SideModuleFirmware/LapCounterControlModule_cmake/app/config/lane_protocol.h
 * Both trees' host tests compare the two copies. Change it in one change,
 * in both places, and bump LANE_PROTO_MINOR (compatible: new registers, new
 * fields at the END of a struct, new enum values) or LANE_PROTO_MAJOR.
 *
 * Background: LANE_FIRMWARE_GUIDE.md §3 and §5, CONTROL_FIRMWARE_GUIDE.md
 * §5.1, SYSTEM_ARCHITECTURE.md §3–§5.
 *
 * ---------------------------------------------------------------------------
 * Transport
 * ---------------------------------------------------------------------------
 * I3C SDR private transfers, controller = control module, targets = lanes.
 *
 *   write:  [reg u8] [payload]                  one private write
 *   read:   [reg u8]  Sr  [payload ...]         private write of the register
 *                                               pointer, repeated START, read
 *
 * All multi-byte values are little-endian. All structs are packed and
 * fixed-size. A write whose length does not match the register's struct is
 * rejected as a whole (the lane NACKs it or ignores it and counts it in
 * laneStatus_t.badWrites); it never half-applies. Writes to CONFIG apply
 * atomically after the transfer.
 *
 * Enumeration: CHAIN + hot-join + ENTDAA (LANE_FIRMWARE_GUIDE §3.2). The
 * lanes have no static address. Lane index n (1..6) gets dynamic address
 * LANE_DYN_ADDR(n). After ENTDAA the control module writes LANE_ID = n.
 *
 * IBI: 1 byte payload, LANE_IBI_* bits. Optional: the control module polls
 * EVENT_FIFO anyway (CONTROL_FIRMWARE_GUIDE §5.1) and never depends on IBIs.
 *
 * Time: every *Us field named "at" or "t" is system time (sysUs) of the
 * control module, which the lane maps from its own timer through SYNC
 * (sync_time). SYNC period = LANE_SYNC_DEFAULT_PERIOD_US unless SYNC_ALIGN
 * says otherwise. Tick k starts at k * period.
 *
 * No persistence on the lane: after every enumeration the control module
 * pushes CONFIG, SYNC_ALIGN and the sampling mode. The lane boots with
 * sensor off, LED off, safe compiled CONFIG defaults.
 */

#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Versions and addresses
 * ------------------------------------------------------------------------- */

#define LANE_PROTO_MAJOR             0U
#define LANE_PROTO_MINOR             1U

#define LANE_MAX_LANES               6U

/* Dynamic address of lane n (1..6): 0x11..0x16. Avoids 0x00-0x07, the
 * hot-join address 0x02 and the 0x7E broadcast family. */
#define LANE_DYN_ADDR_BASE           0x10U
#define LANE_DYN_ADDR(n)             ((uint8_t)(LANE_DYN_ADDR_BASE + (uint8_t)(n)))

#define LANE_SYNC_DEFAULT_PERIOD_US  1000U

/* Board type in the ID register; same numbers as the host protocol's
 * boardType enum (host_protocol.toml). */
#define LANE_BOARD_TYPE_LANE         2U
#define LANE_BOARD_TYPE_LANE_BOOT    5U

/* ---------------------------------------------------------------------------
 * Register pointers
 * ------------------------------------------------------------------------- */

typedef enum {
    LANE_REG_ID          = 0x00,  /* R   laneId_t                                  */
    LANE_REG_LANE_ID     = 0x01,  /* R/W laneIndex_t                               */
    LANE_REG_STATUS      = 0x02,  /* R   laneStatus_t                              */
    LANE_REG_EVENT_FIFO  = 0x03,  /* R   laneFifoHeader_t + count x laneEvent_t, pops */
    LANE_REG_SIGNAL      = 0x04,  /* R   laneSignal_t (diagnostics)                */
    LANE_REG_CONFIG      = 0x05,  /* R/W laneConfig_t                              */
    LANE_REG_LED         = 0x06,  /* W   laneLed_t (immediate)                     */
    LANE_REG_SCHEDULE    = 0x07,  /* W   laneSchedule_t                            */
    LANE_REG_COMMAND     = 0x08,  /* W   laneCommand_t                             */
    LANE_REG_SYNC_ALIGN  = 0x09,  /* W   laneSyncAlign_t                           */
    LANE_REG_RAW_FIFO    = 0x0A,  /* R   laneRawHeader_t + count x u16, pops       */
} laneReg_e;

/* ---------------------------------------------------------------------------
 * IBI payload bits (1 byte)
 * ------------------------------------------------------------------------- */

#define LANE_IBI_EVENTS              0x01U  /* EVENT_FIFO not empty            */
#define LANE_IBI_RAW                 0x02U  /* RAW_FIFO has a full chunk       */
#define LANE_IBI_STATUS              0x04U  /* a STATUS flag or fault changed  */

/* ---------------------------------------------------------------------------
 * Enums (wire values)
 * ------------------------------------------------------------------------- */

/* Lane state machine, LANE_FIRMWARE_GUIDE §3.1. */
typedef enum {
    LANE_STATE_UNENUMERATED = 0,
    LANE_STATE_ENUMERATED   = 1,  /* has an address, waiting for config      */
    LANE_STATE_IDLE         = 2,  /* configured, sampling OFF                */
    LANE_STATE_SAMPLING     = 3,  /* sampling ON or RAW                      */
    LANE_STATE_SLEEP        = 4,
    LANE_STATE_PARKED       = 5,  /* boot-loop guard tripped; see faultId    */
} laneState_e;

typedef enum {
    LANE_SAMPLING_OFF = 0,        /* sensor in shutdown                     */
    LANE_SAMPLING_ON  = 1,        /* detect, emit events                    */
    LANE_SAMPLING_RAW = 2,        /* ON + raw samples into RAW_FIFO         */
} laneSamplingMode_e;

/* Event type; same numbers as the host protocol's laneEventType. */
typedef enum {
    LANE_EVT_CROSSING     = 1,
    LANE_EVT_BLOCKED      = 2,
    LANE_EVT_CLEARED      = 3,
    LANE_EVT_SENSOR_FAULT = 4,
} laneEventType_e;

/* Event flags; same bits as the host protocol's laneEventFlag, and the first
 * three are sync_time's SYNC_TIME_FLAG_* values. */
#define LANE_FLAG_NO_LOCK            0x01U
#define LANE_FLAG_UNALIGNED          0x02U
#define LANE_FLAG_EXTRAPOLATED       0x04U
#define LANE_FLAG_IN_HOLDOFF         0x08U
#define LANE_FLAG_MERGED             0x10U
#define LANE_FLAG_SATURATED          0x20U

/* laneStatus_t.flags */
#define LANE_STATUS_SYNC_LOCKED      0x0001U  /* SYNC period measured            */
#define LANE_STATUS_SYNC_ALIGNED     0x0002U  /* tick index named by SYNC_ALIGN  */
#define LANE_STATUS_SYNC_LOST        0x0004U  /* no edge for several periods     */
#define LANE_STATUS_SENSOR_OK        0x0008U  /* VCNL4040 answers, ID matches    */
#define LANE_STATUS_CONFIG_VALID     0x0010U  /* a CONFIG was written and applied */
#define LANE_STATUS_CALIBRATED       0x0020U  /* baseline established           */
#define LANE_STATUS_IN_HOLDOFF       0x0040U
#define LANE_STATUS_BLOCKED          0x0080U  /* signal below LOW right now      */
#define LANE_STATUS_FIFO_OVERFLOW    0x0100U  /* events were dropped (see droppedEvents) */
#define LANE_STATUS_BUS_IDLE_SAFE    0x0200U  /* bus watchdog put LED/sensor to defaults */

typedef enum {
    LANE_LED_OFF   = 0,
    LANE_LED_ON    = 1,           /* steady at level                        */
    LANE_LED_BLINK = 2,           /* period / on-share / count              */
} laneLedMode_e;

typedef enum {
    LANE_SCHED_ADD        = 1,    /* queue one action                       */
    LANE_SCHED_CANCEL_ALL = 2,    /* drop every pending action              */
} laneScheduleOp_e;

typedef enum {
    LANE_ACT_LED_OFF      = 1,
    LANE_ACT_LED_ON       = 2,    /* param = level 0..1000 (0 = keep current) */
    LANE_ACT_SAMPLING_OFF = 3,
    LANE_ACT_SAMPLING_ON  = 4,
    LANE_ACT_LED_BLINK    = 5,    /* param = period ms, 50 % on, until changed */
} laneAction_e;

#define LANE_SCHEDULE_DEPTH          8U   /* lane queue, >= 4 required */

typedef enum {
    LANE_CMD_SAMPLING         = 1,    /* arg = laneSamplingMode_e           */
    LANE_CMD_CALIBRATE        = 2,    /* re-learn the baseline (lane free)  */
    LANE_CMD_SINGLE_SAMPLE    = 3,    /* one sample into SIGNAL             */
    LANE_CMD_SLEEP            = 4,    /* arg = sleep level, 0 = awake       */
    LANE_CMD_RELEASE_CHAIN    = 5,    /* drop address and CHAIN_OUT         */
    LANE_CMD_RESET            = 6,    /* software reset                     */
    LANE_CMD_CLEAR_COUNTERS   = 7,    /* droppedEvents, badWrites, ...      */
    LANE_CMD_ENTER_BOOTLOADER = 0x7F, /* reserved: future I3C lane bootloader;
                                       * an APP image rejects it (badWrites++) */
} laneCommand_e;

/* ---------------------------------------------------------------------------
 * Register payloads
 * ------------------------------------------------------------------------- */

/* ID (R) */
typedef struct __attribute__((packed)) {
    uint8_t  boardType;        /* LANE_BOARD_TYPE_*                               */
    uint8_t  protoMajor;       /* LANE_PROTO_MAJOR of the image                   */
    uint8_t  protoMinor;
    uint8_t  reserved;
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;
    uint8_t  fwDirty;          /* 1 = built from uncommitted changes              */
    uint32_t fwGitRev;
    uint32_t imageCrc;         /* CRC32 of the running image, 0 = unknown         */
    uint8_t  uid[12];          /* MCU 96-bit unique ID                            */
} laneId_t;

/* LANE_ID (R/W) */
typedef struct __attribute__((packed)) {
    uint8_t  laneIndex;        /* 1..6, 0 = unassigned                            */
} laneIndex_t;

/* STATUS (R). Also the control module's liveness probe. */
typedef struct __attribute__((packed)) {
    uint8_t  state;            /* laneState_e                                     */
    uint8_t  samplingMode;     /* laneSamplingMode_e                              */
    uint16_t flags;            /* LANE_STATUS_*                                   */
    uint8_t  fifoCount;        /* events waiting in EVENT_FIFO                    */
    uint8_t  schedulePending;  /* actions waiting in the SCHEDULE queue           */
    uint16_t nextEventSeq;     /* seq the next event will get                     */
    uint16_t droppedEvents;    /* FIFO overflows since boot / clear (saturates)   */
    uint16_t syncShifts;       /* SYNC_ALIGN disagreed with the running alignment */
    uint16_t badWrites;        /* rejected register writes (saturates)            */
    uint16_t lateActions;      /* scheduled actions that fired late (saturates)   */
    int16_t  lastError;        /* laneStatus_e of the last error, 0 = none        */
    uint8_t  resetCause;       /* same values as the host protocol's resetCause   */
    uint8_t  faultId;          /* fault_guard breadcrumb id of the last fault boot */
    uint32_t uptimeMs;
} laneStatus_t;

/* One event (EVENT_FIFO record). */
typedef struct __attribute__((packed)) {
    uint16_t seq;              /* per-lane counter, wraps; gaps = lost on the lane */
    uint8_t  type;             /* laneEventType_e                                 */
    uint8_t  flags;            /* LANE_FLAG_*                                     */
    uint64_t tEntryUs;         /* sysUs of the interpolated entry (timing reference) */
    uint16_t dwell10us;        /* entry to exit, 10 µs units (saturates)          */
    uint16_t depth;            /* signal minimum below baseline: confidence       */
} laneEvent_t;

/*
 * EVENT_FIFO read: header, then `count` records. The control module reads
 * LANE_FIFO_READ_BYTES. A lane that can end a read early (I3C T-bit) stops
 * after the last record; otherwise it pads with zeros. The lane pops exactly
 * the records whose bytes were fully clocked out.
 */
typedef struct __attribute__((packed)) {
    uint8_t  count;            /* records that follow (0..LANE_FIFO_READ_MAX)     */
    uint8_t  remaining;        /* still queued after these                        */
    uint16_t reserved;
} laneFifoHeader_t;

#define LANE_FIFO_READ_MAX           4U
#define LANE_FIFO_READ_BYTES         (sizeof(laneFifoHeader_t) + LANE_FIFO_READ_MAX * sizeof(laneEvent_t))

/* SIGNAL (R): the detector's view, for diagnostics and calibration. */
typedef struct __attribute__((packed)) {
    uint16_t raw;              /* last PS sample                                  */
    uint16_t baseline;         /* tracked bare-foil level                         */
    uint16_t thrLow;           /* "car present" threshold in use                  */
    uint16_t thrHigh;          /* "car gone" threshold in use                     */
    uint16_t noise;            /* estimated noise (same units as raw)             */
    uint16_t reserved;
    uint64_t sampleUs;         /* sysUs of `raw`                                  */
} laneSignal_t;

/* CONFIG (R/W). Applied as a whole. Out-of-range values reject the write. */
typedef struct __attribute__((packed)) {
    uint32_t samplePeriodUs;        /* sampling period, e.g. 1000 (1 kHz)          */
    uint32_t samplePhaseUs;         /* offset inside the period (stagger IR pulses) */
    uint32_t mergeGapUs;            /* re-entry within this counts as the same car */
    uint32_t blockTimeoutUs;        /* below LOW longer than this → BLOCKED        */
    uint32_t holdoffUs;             /* after an event; 0 = off                     */
    uint32_t holdoffSamplePeriodUs; /* sampling period inside the holdoff; 0 = same */
    uint16_t thrDropPermille;       /* LOW = baseline × (1000 − x) / 1000           */
    uint16_t hystPermille;          /* HIGH = LOW + baseline × x / 1000             */
    uint16_t minDwell10us;          /* shorter dips are glitches, not cars          */
    uint16_t rawDecimation;         /* RAW mode: keep every Nth sample (>= 1)       */
    uint8_t  irCurrent;             /* VCNL4040 LED_I code (PS_MS register)          */
    uint8_t  psIntegration;         /* VCNL4040 PS_IT code (PS_CONF1)               */
    uint8_t  psMultiPulse;          /* VCNL4040 PS_MPS code (PS_CONF3)              */
    uint8_t  reserved;
} laneConfig_t;

/* LED (W): applies at once. */
typedef struct __attribute__((packed)) {
    uint8_t  mode;             /* laneLedMode_e                                   */
    uint8_t  reserved;
    uint16_t level;            /* 0..1000 perceived brightness (gamma on the lane) */
    uint16_t blinkPeriodMs;
    uint16_t blinkOnPermille;
    uint16_t blinkCount;       /* 0 = until changed                               */
    uint16_t reserved2;
} laneLed_t;

/* SCHEDULE (W): one action at a system time, or cancel all. A due time that
 * already passed fires at once and counts in lateActions. Actions fire in
 * time order. Queue full rejects the write (badWrites). */
typedef struct __attribute__((packed)) {
    uint8_t  op;               /* laneScheduleOp_e                                */
    uint8_t  action;           /* laneAction_e (ignored for CANCEL_ALL)           */
    uint16_t param;            /* per action, see laneAction_e                    */
    uint32_t reserved;
    uint64_t atUs;             /* sysUs                                           */
} laneSchedule_t;

/* COMMAND (W) */
typedef struct __attribute__((packed)) {
    uint8_t  cmd;              /* laneCommand_e                                   */
    uint8_t  arg;
    uint16_t reserved;
} laneCommand_t;

/* SYNC_ALIGN (W): names the control module's tick. Measured just before the
 * transfer starts; its latency to the lane must stay below period / 2. The
 * lane passes tick and phaseUs to syncTimeAlign(). A periodUs different from
 * the running one resets the lane's lock first. */
typedef struct __attribute__((packed)) {
    uint32_t tick;             /* tick of the control module's last SYNC edge     */
    uint32_t phaseUs;          /* µs since that edge                              */
    uint32_t periodUs;         /* SYNC period (LANE_SYNC_DEFAULT_PERIOD_US)       */
} laneSyncAlign_t;

/* RAW_FIFO read: header + `count` u16 samples; sample i was taken at
 * firstUs + i × periodUs. Same early-end / zero-pad rule as EVENT_FIFO. */
typedef struct __attribute__((packed)) {
    uint8_t  count;            /* samples that follow (0..LANE_RAW_READ_MAX)       */
    uint8_t  reserved;
    uint16_t chunkSeq;         /* counts chunks; gaps = dropped on the lane       */
    uint32_t periodUs;         /* after decimation                                */
    uint64_t firstUs;          /* sysUs of the first sample                       */
} laneRawHeader_t;

#define LANE_RAW_READ_MAX            32U
#define LANE_RAW_READ_BYTES          (sizeof(laneRawHeader_t) + LANE_RAW_READ_MAX * sizeof(uint16_t))

/* ---------------------------------------------------------------------------
 * Size checks: the wire layout must never change by accident.
 * ------------------------------------------------------------------------- */

_Static_assert(sizeof(laneId_t)         == 28, "laneId_t");
_Static_assert(sizeof(laneIndex_t)      ==  1, "laneIndex_t");
_Static_assert(sizeof(laneStatus_t)     == 24, "laneStatus_t");
_Static_assert(sizeof(laneEvent_t)      == 16, "laneEvent_t");
_Static_assert(sizeof(laneFifoHeader_t) ==  4, "laneFifoHeader_t");
_Static_assert(sizeof(laneSignal_t)     == 20, "laneSignal_t");
_Static_assert(sizeof(laneConfig_t)     == 36, "laneConfig_t");
_Static_assert(sizeof(laneLed_t)        == 12, "laneLed_t");
_Static_assert(sizeof(laneSchedule_t)   == 16, "laneSchedule_t");
_Static_assert(sizeof(laneCommand_t)    ==  4, "laneCommand_t");
_Static_assert(sizeof(laneSyncAlign_t)  == 12, "laneSyncAlign_t");
_Static_assert(sizeof(laneRawHeader_t)  == 16, "laneRawHeader_t");
