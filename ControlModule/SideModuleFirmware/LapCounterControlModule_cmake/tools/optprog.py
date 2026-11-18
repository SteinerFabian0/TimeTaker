#!/usr/bin/env python3
"""Program the option-byte policy for the TimeTaker control module. Once per chip.

    python3 tools/optprog.py             # program whatever is not already right
    python3 tools/optprog.py --dry-run   # decode and report, write nothing

What it sets (CONTROL_FIRMWARE_GUIDE §5.6 item 4):

  BOOT_SEL         NOT written by default. Polarity is unverified (see below):
                   pass --boot-sel 1 only after the bench check (BENCH_TESTS F-4).
                   With it, BOOT_SEL = 1 means: the BOOT0 PIN decides the boot source. This board has a real
                   10 k pull-down on BOOT0 (R201) and the SW604 BOOT button, so
                   the pin must stay in charge: SW604 + SW605 (NRST) is the ROM
                   USB DFU recovery path. Value per the C5 LL/HAL headers
                   (LL_FLASH_OB_BOOT0_BOOTPIN = BOOT_SEL set,
                   HAL_FLASH_ITF_OB_BOOT_PIN). NOTE: the Yuhka C542 hub script
                   says the opposite meaning for BOOT_SEL = 1; it is NOT copied.
                   Verify on the first board (BENCH_TESTS.md F-4).
  IWDG_STDBY = 0   IWDG frozen in Standby. The control module never enters
                   Standby; this is belt and braces, as the architecture asks.
  BOOT0            not touched (only matters with BOOT_SEL = 0).

Deliberately NOT touched: IWDG_SW (software start, the firmware starts it),
IWDG_STOP, RDP, WRP. (v1 has no bootloader, so there are no pages to
write-protect; every image goes on over SWD.)

  * Option bytes reload ONLY on a power-on reset: unplug USB (and the probe's
    3V3 if it powers the board) once after this runs.
  * Per chip, not per image. The firmware reports ERR_OPTION_BYTES as its last
    error (STATUS) on a chip that is not programmed; it still boots.
"""

import argparse
import os
import shutil
import sys
import time
from pathlib import Path


def _reexec_under_pyocd_python():
    """pyocd is usually a pipx install, so the system python cannot import it.

    Find the interpreter behind whatever `pyocd` is on PATH (pipx venv, plain
    venv, system) and hand the script over to it, so `python3 tools/optprog.py`
    works on any machine without a venv dance.
    """
    if os.environ.get("OPTPROG_REEXEC"):
        sys.exit("pyocd is not importable from the interpreter that runs `pyocd`.\n"
                 "Install it:  pipx install pyocd")

    exe = shutil.which("pyocd")
    if exe is None:
        sys.exit("pyocd not found. Install it:  pipx install pyocd")

    with open(exe, "rb") as handle:
        shebang = handle.readline()
    if not shebang.startswith(b"#!"):
        sys.exit(f"{exe} is not a python entry point; run this under a python that has pyocd")

    interpreter = shebang[2:].strip().decode()
    os.environ["OPTPROG_REEXEC"] = "1"
    os.execv(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:])


try:
    from pyocd.core.helpers import ConnectHelper
except ImportError:
    _reexec_under_pyocd_python()


TARGET = os.environ.get("C5_TARGET", "stm32c542ccu6")

# H5-style FLASH, base 0x40022000 (from STM32C542.svd).
FLASH = 0x40022000
OPTKEYR = FLASH + 0x0C
OPTCR = FLASH + 0x1C
SR = FLASH + 0x20
OPTSR_CUR = FLASH + 0x50
OPTSR_PRG = FLASH + 0x54

OPTKEY1 = 0x08192A3B
OPTKEY2 = 0x4C5D6E7F

OPTLOCK = 1 << 0
OPTSTRT = 1 << 1
SR_BSY = 1 << 0
OPTCHANGEERR = 1 << 23

RDP_LEVEL_SHIFT = 8
RDP_LEVEL_MASK = 0xFF << RDP_LEVEL_SHIFT

# Hamming(8,4) codes, not the classic 0xAA/0xCC — see LL_FLASH_OB_RDP_LEVEL_* in
# stm32c5xx_ll_flash.h. Anything not listed is level 1.
RDP_LEVEL_0 = 0xED
RDP_LEVELS = {0xED: "level 0, unprotected", 0xD1: "LEVEL 2 + boundary scan", 0x72: "LEVEL 2"}

IWDG_STDBY = 21
BOOT_SEL = 22
BOOT0 = 23

# bit -> level this board wants. Everything outside this dict is preserved.
POLICY = {IWDG_STDBY: 0}   # BOOT_SEL only with --boot-sel (polarity unverified)

# bit -> (name, meaning at 0, meaning at 1), for the report.
BITS = {
    3:  ("IWDG_SW", "hardware-started", "software-started"),
    4:  ("WWDG_SW", "hardware-started", "software-started"),
    6:  ("NRST_STOP", "reset on Stop entry", "no reset on Stop entry"),
    7:  ("NRST_STDBY", "reset on Standby entry", "no reset on Standby entry"),
    20: ("IWDG_STOP", "frozen in Stop", "running in Stop"),
    21: ("IWDG_STDBY", "frozen in Standby", "RUNNING in Standby"),
    22: ("BOOT_SEL", "BOOT0 from the option bit", "BOOT0 from the pin (LL_FLASH_OB_BOOT0_BOOTPIN)"),
    23: ("BOOT0", "main flash", "ROM bootloader"),
}


def find_pack():
    """The DFP ships in this project; fall back to a search so cwd never matters."""
    override = os.environ.get("C5_PACK")
    if override:
        return Path(override)

    project = Path(__file__).resolve().parent.parent
    packs = sorted(project.glob("*stm32c5xx_dfp*.pack")) or \
        sorted(project.rglob("*stm32c5xx_dfp*.pack"))
    if not packs:
        sys.exit(f"no stm32c5xx DFP .pack found under {project} (set C5_PACK)")
    return packs[0]


def decode(value):
    lines = []
    for bit in sorted(BITS):
        name, at_zero, at_one = BITS[bit]
        level = (value >> bit) & 1
        meaning = at_one if level else at_zero
        if bit in POLICY:
            mark = "  [ok]" if level == POLICY[bit] else "  [WRONG]"
        else:
            mark = "  (not managed)"
        lines.append(f"    {name:<11} = {level}   {meaning:<31}{mark}")
    rdp = (value & RDP_LEVEL_MASK) >> RDP_LEVEL_SHIFT
    state = RDP_LEVELS.get(rdp, "LEVEL 1, flash readout protected")
    lines.append(f"    {'RDP_LEVEL':<11} = 0x{rdp:02x} ({state})")
    return "\n".join(lines)


def apply_policy(value):
    for bit, want in POLICY.items():
        value = (value | (1 << bit)) if want else (value & ~(1 << bit))
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dry-run", action="store_true",
                        help="decode and report, write nothing")
    parser.add_argument("--target", default=TARGET, help=f"pyocd target (default {TARGET})")
    parser.add_argument("--frequency", type=int, default=1_000_000,
                        help="SWD clock in Hz (default 1 MHz)")
    parser.add_argument("--boot-sel", type=int, choices=(0, 1), default=None,
                        help="also write BOOT_SEL (1 = BOOT0 pin decides). Opt-in: polarity is "
                             "bench-verified first (BENCH_TESTS F-4)")
    args = parser.parse_args()
    if args.boot_sel is not None:
        POLICY[BOOT_SEL] = args.boot_sel

    pack = find_pack()
    print("board  TimeTaker control module (C542) - IWDG_STDBY frozen; BOOT_SEL "
          + ("not managed" if args.boot_sel is None else f"-> {args.boot_sel}"))
    print(f"pack   {pack}")
    print(f"target {args.target}")

    session = ConnectHelper.session_with_chosen_probe(options={
        "pack": str(pack),
        "target_override": args.target,
        # attach, not reset: latch onto whatever is running, including the ROM
        # bootloader on a board that still boots from a floating BOOT0.
        "connect_mode": "attach",
        "frequency": args.frequency,
    })
    if session is None:
        sys.exit("no probe found")

    with session:
        t = session.target
        t.halt()

        cur = t.read32(OPTSR_CUR)   # active: loaded at the last power-on
        prg = t.read32(OPTSR_PRG)   # stored: what the next power-on will load

        print(f"\nOPTSR_CUR (active) = 0x{cur:08x}\n{decode(cur)}")
        print(f"\nOPTSR_PRG (stored) = 0x{prg:08x}\n{decode(prg)}")

        rdp = (prg & RDP_LEVEL_MASK) >> RDP_LEVEL_SHIFT
        if rdp != RDP_LEVEL_0:
            sys.exit(f"\nRDP_LEVEL is 0x{rdp:02x}, not level 0 (0xED). Refusing to touch "
                     "option bytes on a protected part — a wrong write can mass-erase it.")

        desired = apply_policy(prg)
        stored_ok = desired == prg
        active_ok = apply_policy(cur) == cur

        if stored_ok and active_ok:
            print("\nThis chip is done — policy stored AND active.")
            return 0

        if stored_ok:
            print("\nPolicy is already stored but NOT active.")
            print(">> FULLY power-cycle this board (USB out, probe power off).")
            return 0

        if args.dry_run:
            print(f"\nWould write OPTSR_PRG = 0x{desired:08x} (dry run, nothing done).")
            return 0

        if t.read32(OPTCR) & OPTLOCK:
            t.write32(OPTKEYR, OPTKEY1)
            t.write32(OPTKEYR, OPTKEY2)
        if t.read32(OPTCR) & OPTLOCK:
            sys.exit("failed to unlock option bytes (OPTLOCK still set)")

        t.write32(OPTSR_PRG, desired)
        t.write32(OPTCR, t.read32(OPTCR) | OPTSTRT)

        for _ in range(2000):
            if not (t.read32(SR) & SR_BSY):
                break
            time.sleep(0.001)
        else:
            sys.exit("timeout waiting for option programming to finish")

        sr = t.read32(SR)
        if sr & OPTCHANGEERR:
            t.write32(SR, OPTCHANGEERR)   # w1c
            sys.exit(f"OPTCHANGEERR set (SR=0x{sr:08x}) — option write rejected")

        written = t.read32(OPTSR_PRG)
        print(f"\nOPTSR_PRG (new)    = 0x{written:08x}\n{decode(written)}")
        if written != desired:
            sys.exit(f"readback mismatch: wanted 0x{desired:08x}")

        print("\nOK — option bytes written.")
        print(">> Now FULLY power-cycle this board (USB out, probe power off).")
        print(">> Re-run this script afterwards to confirm it went active.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
