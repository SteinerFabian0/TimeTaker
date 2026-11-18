/*
 * LANE_STATUS.flags bits: lane_protocol.h LANE_STATUS_* (the host protocol
 * passes the lane's STATUS register through; the generator does not know
 * them). Keep in step with LaneModule/.../app/config/lane_protocol.h.
 */

export const LaneStatusFlag = {
  /** SYNC period measured. */
  SYNC_LOCKED: 0x0001,
  /** Tick index named by SYNC_ALIGN. */
  SYNC_ALIGNED: 0x0002,
  /** No SYNC edge for several periods. */
  SYNC_LOST: 0x0004,
  /** VCNL4040 answers, ID matches. */
  SENSOR_OK: 0x0008,
  /** A CONFIG was written and applied. */
  CONFIG_VALID: 0x0010,
  /** Baseline established (not learning). */
  CALIBRATED: 0x0020,
  IN_HOLDOFF: 0x0040,
  /** Signal below LOW right now. */
  BLOCKED: 0x0080,
  /** Events were dropped (see droppedEvents). */
  FIFO_OVERFLOW: 0x0100,
  /** Bus watchdog put LED/sensor to defaults. */
  BUS_IDLE_SAFE: 0x0200,
} as const;
export type LaneStatusFlag = (typeof LaneStatusFlag)[keyof typeof LaneStatusFlag];

/** Flag names set in `flags`, for display. */
export function laneStatusFlagNames(flags: number): string[] {
  return Object.entries(LaneStatusFlag).filter(([, v]) => flags & v).map(([k]) => k);
}
