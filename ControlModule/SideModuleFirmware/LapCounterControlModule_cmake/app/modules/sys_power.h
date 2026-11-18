/*
 * System power: the super-loop's idle, and nothing else.
 *
 * Port of the hub's sys_power, cut down to sysPowerIdle(). The control module
 * is USB-powered and NEVER enters Stop or Standby: TIM12 stops in Stop1, and
 * with it the system clock every timestamp lives on (sys_time.h). Plain WFI
 * Sleep keeps every peripheral, DMA, SysTick and TIM12 running; any interrupt
 * (the 1 ms SysTick or the 1 kHz TIM12 tick at the latest) resumes the loop
 * within a few cycles. A flag an ISR sets just before the WFI is therefore
 * seen at most about 1 ms later, which no deadline here cares about.
 *
 * Context: main loop only (last call of ctrlRun()).
 */

#pragma once

void sysPowerIdle(void);
