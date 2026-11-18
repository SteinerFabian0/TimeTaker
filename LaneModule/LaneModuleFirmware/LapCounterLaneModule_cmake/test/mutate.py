#!/usr/bin/env python3
# Mutation check of the host suites (test/README.md): each mutation breaks
# exactly one rule in a temporary copy of app/, and the suite that guards the
# rule must then fail. The real tree is never touched.
#
#   test/mutate.py            prints every mutation; "SURVIVED" = a rule no
#                             test holds; exit status = survivors + bad patterns
#
# A "BADPATTERN" means the source moved: update the pattern with the code.
import os, shutil, subprocess, sys, tempfile
LANE = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M = 'app/modules/'
SUITES = {
 'host_detect': [M+'lane_detect.c'],
 'host_event_fifo': [M+'event_fifo.c'],
 'host_schedule': [M+'schedule_queue.c'],
 'host_regs': [M+'lane_regs.c', M+'image_crc.c'],
 'host_chain': [M+'chain_logic.c'],
}
D, E, S, R, C = M+'lane_detect.c', M+'event_fifo.c', M+'schedule_queue.c', M+'lane_regs.c', M+'chain_logic.c'
muts = [
 # --- detection
 ('host_detect', D, 'd->baselineQ8 = (uint32_t)((int32_t)d->baselineQ8 + err / (1 << DETECT_BASELINE_SHIFT));', '(void)err;', 'baseline tracks while FREE'),
 ('host_detect', D, '<= d->cfg.mergeGapUs) {', '<= 0U) {', 'merge gap merges'),
 ('host_detect', D, 'if (dwell < d->cfg.minDwellUs) {', 'if (false) {', 'min dwell rejects glitches'),
 ('host_detect', D, 'if ((out->flags & LANE_FLAG_IN_HOLDOFF) == 0U) {', 'if (true) {', 'flagged event does not extend holdoff'),
 ('host_detect', D, 'out->flags |= LANE_FLAG_IN_HOLDOFF;', '(void)0;', 'holdoff flags'),
 ('host_detect', D, 'laneDetectStartPass(d, entry, raw, sat || prevSat);', 'laneDetectStartPass(d, entry, raw, sat);', 'saturated entry sample flags'),
 ('host_detect', D, '>= d->cfg.blockTimeoutUs) {', '>= 0xFFFFFFFFU) {', 'block timeout emits BLOCKED'),
 ('host_detect', D, '? laneDetectCrossFall(prevT, prevRaw, t, raw, d->thrLow) : t;', '? t : t;', 'entry interpolated'),
 ('host_detect', D, 'd->gapT       = laneDetectCrossRise(prevT, prevRaw, t, raw, d->thrHigh);', 'd->gapT       = t;', 'exit interpolated'),
 ('host_detect', D, 'laneDetectFill(d, LANE_EVT_CLEARED, d->gapT - d->entryT, out);', 'laneDetectFill(d, LANE_EVT_CLEARED, 0U, out);', 'CLEARED carries the blockage'),
 ('host_detect', D, 'if (d->learnCount >= DETECT_LEARN_SAMPLES) {', 'if (d->learnCount >= 1U) {', 'baseline learned over 64 samples'),
 ('host_detect', D, '&& cfg->hystPermille < cfg->thrDropPermille', '', 'config: hysteresis < drop'),
 ('host_detect', D, '&& cfg->blockTimeoutUs > cfg->mergeGapUs', '', 'config: block > merge'),
 ('host_detect', D, '&& cfg->minDwellUs < cfg->blockTimeoutUs', '', 'config: dwell < block'),
 ('host_detect', D, '&& cfg->saturationRaw > 0U', '', 'config: saturation > 0'),

 ('host_detect', D, 'return d->state == DETECT_STATE_IN_CAR || d->state == DETECT_STATE_BLOCKED;', 'return d->state == DETECT_STATE_BLOCKED;', 'IsBlocked during a pass'),
 # --- event fifo
 ('host_event_fifo', E, '        f->overflowed = true;\n        return LANE_ERR_FIFO_FULL;', '        f->overflowed = true;\n        f->nextSeq--;\n        return LANE_ERR_FIFO_FULL;', 'dropped event spends a seq'),
 ('host_event_fifo', E, 'if (f->count >= EVENT_FIFO_DEPTH) {', 'if (f->count > EVENT_FIFO_DEPTH) {', 'full keeps the oldest'),
 ('host_event_fifo', E, 'if (f->dropped < 0xFFFFU) {', 'if (true) {', 'drop count saturates'),
 ('host_event_fifo', E, 'n = (clockedOut - sizeof(laneFifoHeader_t)) / sizeof(laneEvent_t);', 'n = (clockedOut - sizeof(laneFifoHeader_t) + 15U) / sizeof(laneEvent_t);', 'pop only fully clocked records'),
 ('host_event_fifo', E, '    if (n > offered) {\n        n = offered;\n    }\n    if (n > f->count)', '    if (n > f->count)', 'never pop more than offered'),
 ('host_event_fifo', E, '.remaining = (uint8_t)(f->count - n),', '.remaining = 0U,', 'header remaining'),
 ('host_event_fifo', E, 'uint8_t n = (f->count < LANE_FIFO_READ_MAX) ? f->count : (uint8_t)LANE_FIFO_READ_MAX;', 'uint8_t n = f->count;', 'read offers at most 4'),
 ('host_event_fifo', E, 'f->overflowed = false;', '(void)0;', 'clear resets overflow flag'),
 ('host_event_fifo', E, 'return diff <= periodUs / 4U;', 'return true;', 'raw: off-grid sample starts a chunk'),
 ('host_event_fifo', E, '            f->dropping = true;\n            f->nextChunkSeq++;', '            f->dropping = true;', 'raw: dropping skips a chunkSeq'),
 ('host_event_fifo', E, '    c->firstUs += (uint64_t)n * c->periodUs;', '', 'raw: partial pop advances firstUs'),
 ('host_event_fifo', E, 'if (c->closed || c->count >= LANE_RAW_READ_MAX || c->periodUs != periodUs) {', 'if (c->closed || c->count >= LANE_RAW_READ_MAX) {', 'raw: period change starts a chunk'),
 # --- schedule
 ('host_schedule', S, 'while (pos > 0U && q->entries[pos - 1U].atUs > atUs) {', 'while (false) {', 'actions sorted by time'),
 ('host_schedule', S, 'q->entries[pos - 1U].atUs > atUs', 'q->entries[pos - 1U].atUs >= atUs', 'equal times keep order'),
 ('host_schedule', S, 'if (q->count == 0U || q->entries[0].atUs > nowUs) {', 'if (q->count == 0U) {', 'not due before its time'),
 ('host_schedule', S, '*lateUs = nowUs - out->atUs;', '*lateUs = 0U;', 'lateness reported'),
 ('host_schedule', S, '    q->count = 0U;\n}', '    (void)q;\n}', 'cancel-all empties'),
 ('host_schedule', S, 'if (q->count >= LANE_SCHEDULE_DEPTH) {', 'if (q->count >= LANE_SCHEDULE_DEPTH + 1U) {', 'full rejects'),
 ('host_schedule', S, '    q->count--;\n    memmove', '    memmove', 'popped action removed'),
 # --- chain
 ('host_chain', C, '(uint32_t)(nowMs - c->rawSinceMs) < CHAIN_DEBOUNCE_MS', '(uint32_t)(nowMs - c->rawSinceMs) < 0U', 'debounce'),
 ('host_chain', C, '    c->hasAddress = false;\n    return CHAIN_EVENT_FELL;', '    return CHAIN_EVENT_FELL;', 'fall forgets address'),
 ('host_chain', C, 'return c->hasAddress && c->stableHigh;', 'return c->hasAddress;', 'CHAIN_OUT needs CHAIN_IN'),
 ('host_chain', C, 'return c->stableHigh && !c->hasAddress &&', 'return !c->hasAddress &&', 'join only while CHAIN_IN high'),
 ('host_chain', C, 'return c->stableHigh && !c->hasAddress && (int32_t)(nowMs - c->nextJoinMs) >= 0;', 'return c->stableHigh && (int32_t)(nowMs - c->nextJoinMs) >= 0;', 'no join with an address'),
 ('host_chain', C, '    c->backoffMs *= 2U;', '', 'backoff doubles'),
 ('host_chain', C, 'if (c->backoffMs > CHAIN_JOIN_BACKOFF_MAX_MS) {', 'if (false) {', 'backoff capped'),
 ('host_chain', C, 'c->nextJoinMs = nowMs + CHAIN_REJOIN_HOLD_MS;', 'c->nextJoinMs = nowMs;', 'RSTDAA hold'),

 ('host_chain', C, '        c->nextJoinMs = nowMs;\n        return CHAIN_EVENT_ROSE;', '        return CHAIN_EVENT_ROSE;', 'join at once on rise'),
]
# --- register map: every validation rule, one at a time
cfg_rules = [

 'c->samplePeriodUs <= LANE_CFG_SAMPLE_PERIOD_US_MAX\n              && ',
 '&& c->samplePhaseUs < c->samplePeriodUs',
 '&& c->mergeGapUs <= LANE_CFG_MERGE_GAP_US_MAX',
 '&& c->blockTimeoutUs > c->mergeGapUs',
 '&& c->blockTimeoutUs <= LANE_CFG_BLOCK_TIMEOUT_US_MAX',
 '&& c->holdoffUs <= LANE_CFG_HOLDOFF_US_MAX',
 '(c->holdoffSamplePeriodUs >= c->samplePeriodUs\n                      && ',
 '\n                      && c->holdoffSamplePeriodUs <= LANE_CFG_HOLDOFF_SAMPLE_PERIOD_US_MAX',

 '&& c->thrDropPermille <= LANE_CFG_THR_DROP_PERMILLE_MAX',
 '&& c->hystPermille < c->thrDropPermille',
 '&& (uint32_t)c->minDwell10us * 10U < c->blockTimeoutUs',
 '&& c->rawDecimation >= LANE_CFG_RAW_DECIMATION_MIN && ',
 '&& c->rawDecimation <= LANE_CFG_RAW_DECIMATION_MAX',
 '&& c->irCurrent <= LANE_CFG_IR_CURRENT_MAX',
 '&& c->psIntegration <= LANE_CFG_PS_INTEGRATION_MAX',
 '&& c->psMultiPulse <= LANE_CFG_PS_MULTI_PULSE_MAX',
]
for r in cfg_rules:
    new = '&& ' if r.startswith('&& c->rawDecimation >= LANE_CFG_RAW_DECIMATION_MIN') else ('(' if r.startswith('(c->holdoff') else '')
    muts.append(('host_regs', R, r, new, 'CONFIG rule: ' + r.strip().replace('\n', ' ')[:50]))
muts += [
 ('host_regs', R, 'if (ok && c->samplePeriodUs < laneRegsMinSamplePeriodUs(c->psIntegration, c->psMultiPulse)) {', 'if (false) {', 'CONFIG period fits the conversion'),
 ('host_regs', R, 'if (l->mode > (uint8_t)LANE_LED_BLINK || l->level > LED_LEVEL_MAX) {', 'if (l->level > LED_LEVEL_MAX) {', 'LED mode'),
 ('host_regs', R, 'if (l->mode > (uint8_t)LANE_LED_BLINK || l->level > LED_LEVEL_MAX) {', 'if (l->mode > (uint8_t)LANE_LED_BLINK) {', 'LED level'),
 ('host_regs', R, '(l->blinkPeriodMs < LED_BLINK_PERIOD_MIN_MS || ', '(', 'LED blink period min'),
 ('host_regs', R, ' || l->blinkPeriodMs > LED_BLINK_PERIOD_MAX_MS', '', 'LED blink period max'),
 ('host_regs', R, '|| l->blinkOnPermille < 1U || ', '|| ', 'LED on-share min'),
 ('host_regs', R, ' || l->blinkOnPermille > 999U)', ')', 'LED on-share max'),
 ('host_regs', R, '    if (s->op != (uint8_t)LANE_SCHED_ADD) {\n        return LANE_ERR_REG_VALUE;\n    }', '', 'SCHEDULE op'),
 ('host_regs', R, 'return (s->param <= LED_LEVEL_MAX) ? LANE_OK : LANE_ERR_REG_VALUE;', 'return LANE_OK;', 'SCHEDULE LED_ON level'),
 ('host_regs', R, '(s->param >= LED_BLINK_PERIOD_MIN_MS && s->param <= LED_BLINK_PERIOD_MAX_MS)', '(s->param <= LED_BLINK_PERIOD_MAX_MS)', 'SCHEDULE blink min'),
 ('host_regs', R, '(s->param >= LED_BLINK_PERIOD_MIN_MS && s->param <= LED_BLINK_PERIOD_MAX_MS)', '(s->param >= LED_BLINK_PERIOD_MIN_MS)', 'SCHEDULE blink max'),
 ('host_regs', R, '    default:\n        return LANE_ERR_REG_VALUE;\n    }\n}\n\nstatic laneStatus_e laneRegsValidateCommand', '    default:\n        return LANE_OK;\n    }\n}\n\nstatic laneStatus_e laneRegsValidateCommand', 'SCHEDULE action'),
 ('host_regs', R, 'return (c->arg <= (uint8_t)LANE_SAMPLING_RAW) ? LANE_OK : LANE_ERR_REG_VALUE;', 'return LANE_OK;', 'COMMAND sampling mode'),
 ('host_regs', R, 'if (c->arg <= (uint8_t)LANE_SLEEP_IDLE) {', 'if (c->arg <= (uint8_t)LANE_SLEEP_STANDBY) {', 'COMMAND sleep levels'),
 ('host_regs', R, 'return (c->arg <= (uint8_t)LANE_SLEEP_STANDBY) ? LANE_ERR_CMD_UNSUPPORTED : LANE_ERR_REG_VALUE;', 'return LANE_ERR_CMD_UNSUPPORTED;', 'COMMAND sleep level range'),
 ('host_regs', R, '    case LANE_CMD_ENTER_BOOTLOADER:\n        /* Reserved for the future I3C lane bootloader; an APP image refuses. */\n        return LANE_ERR_CMD_UNSUPPORTED;', '    case LANE_CMD_ENTER_BOOTLOADER:\n        return LANE_OK;', 'ENTER_BOOTLOADER rejected'),
 ('host_regs', R, '    default:\n        return LANE_ERR_REG_VALUE;\n    }\n}\n\nstatic laneStatus_e laneRegsValidateSyncAlign', '    default:\n        return LANE_OK;\n    }\n}\n\nstatic laneStatus_e laneRegsValidateSyncAlign', 'COMMAND unknown'),
 ('host_regs', R, 'if (s->periodUs < SYNC_TIME_PERIOD_MIN_US || ', 'if (', 'SYNC_ALIGN period min'),
 ('host_regs', R, ' || s->periodUs > SYNC_TIME_PERIOD_MAX_US', '', 'SYNC_ALIGN period max'),
 ('host_regs', R, '\n        || s->phaseUs >= s->periodUs) {', ') {', 'SYNC_ALIGN phase'),
 ('host_regs', R, 'return (w->u.laneIndex.laneIndex <= LANE_MAX_LANES) ? LANE_OK : LANE_ERR_REG_VALUE;', 'return LANE_OK;', 'LANE_ID range'),
 ('host_regs', R, '    if (!laneRegsIsKnown(reg)) {\n        status = LANE_ERR_REG_UNKNOWN;\n    } else if', '    if', 'unknown register'),
 ('host_regs', R, '    } else if (size == 0U) {\n        status = LANE_ERR_REG_READ_ONLY;\n', '', 'read-only register'),
 ('host_regs', R, '    } else if (len - 1U != size) {\n        status = LANE_ERR_REG_LENGTH;\n', '', 'exact length'),
 ('host_regs', R, '    if (status != LANE_OK) {\n        laneRegsReject(r, status);\n    }\n    return status;', '    return status;', 'rejections counted'),
 ('host_regs', R, 'if (r->badWrites < 0xFFFFU) {', 'if (true) {', 'badWrites saturates'),
 ('host_regs', M+'image_crc.c', '0xEDB88320U', '0x04C11DB7U', 'CRC polynomial'),
]

def run_suite(root, suite):
    out = tempfile.mkdtemp(dir=work)
    t = os.path.join(root, 'test')
    srcs = [os.path.join(root, s) for s in SUITES[suite]]
    cmd = ['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-DRELEASE', '-DLANE_HOST_TEST',
           '-I'+t+'/shim', '-I'+root+'/app/config', '-I'+root+'/app/modules',
           '-o', out+'/x', t+'/'+suite+'/main.c', t+'/shim/host_hal.c'] + srcs
    if subprocess.run(cmd, capture_output=True).returncode != 0:
        return 'build-fail'
    try:
        r = subprocess.run([out+'/x'], capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        return 'timeout'
    return 'pass' if r.returncode == 0 else 'fail'

work = tempfile.mkdtemp()
root = os.path.join(work, 'lane')
shutil.copytree(os.path.join(LANE, 'app'), os.path.join(root, 'app'))
shutil.copytree(os.path.join(LANE, 'test'), os.path.join(root, 'test'))
for s in SUITES:
    assert run_suite(root, s) == 'pass', s
killed = survived = bad = 0
for suite, f, old, new, name in muts:
    p = os.path.join(root, f)
    orig = open(p).read()
    if orig.count(old) != 1:
        print('BADPATTERN %-14s %s (%d matches)' % (suite, name, orig.count(old))); bad += 1; continue
    open(p, 'w').write(orig.replace(old, new))
    res = run_suite(root, suite)
    open(p, 'w').write(orig)
    if res == 'pass':
        survived += 1; print('SURVIVED   %-16s %s' % (suite, name))
    else:
        killed += 1; print('killed     %-16s %s (%s)' % (suite, name, res))
print('mutations: %d killed, %d survived, %d bad patterns' % (killed, survived, bad))
shutil.rmtree(work, ignore_errors=True)
sys.exit(survived + bad)
