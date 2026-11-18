/**
******************************************************************************
* @file    stm32c542xc_flash.ld
* @brief   Linker File
******************************************************************************
* @attention
*
* Copyright (c) 2026 STMicroelectronics.
* All rights reserved.
*
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

HEAP_SIZE = 0x200;
STACK_SIZE = 0x1000;  /* 4 KB, raised from the CubeMX 1 KB for the control app: the request path
                         uses ~550 B (host_session) and up to four ISR priority levels nest on top
                         (FPU frames are 104 B each). */


/* MEMORY comes from ctrl_memory.ld, found on the linker search path that
 * cmake/ctrl_app.cmake sets per image (cmake/ld/<layout>/ctrl_memory.ld):
 *   app        ROM 0x0800_0000, 240 KB   standalone bring-up image (default)
 *   app_boot   ROM 0x0800_6000, 216 KB   app behind the bootloader
 *   boot       ROM 0x0800_0000,  24 KB   the bootloader (ctrl_boot)
 * The last two 8 KB pages (0x0803_C000) are the settings and belong to no
 * image (image_header.h has the map). Added for the control app. */
INCLUDE ctrl_memory.ld

SECTIONS
{
  .vectors :
  {
    . = ALIGN(8);
    KEEP(*(.vectors));
    . = ALIGN(8);
  } > ROM

  /* Firmware image header at a fixed +0x400 (image_header.h): the vector
   * table stays at the image base for VTOR, the header follows it. Filled by
   * tools/image_stamp.py after the link. Empty in the bootloader. */
  .image_header (ORIGIN(ROM) + 0x400) :
  {
    KEEP(*(.image_header))
    . = ALIGN(16);
  } > ROM

  ASSERT(SIZEOF(.vectors) <= 0x400, "vector table runs into the image header")

  .text :
  {
    . = ALIGN(8);
    *(.text);
    *(.text*);
    *(.glue_7);
    *(.glue_7t);
    *(.eh_frame);
    KEEP (*(.init));
    KEEP (*(.fini));
    . = ALIGN(8);
    _etext = .;
  } > ROM

  .rodata :
  {
    . = ALIGN(8);
    *(.rodata);
    *(.rodata*);
    . = ALIGN(8);
  } > ROM

  .ARM.extab (READONLY) :
  {
    . = ALIGN(8);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(8);
  } > ROM

  .ARM (READONLY) :
  {
    . = ALIGN(8);
    __exidx_start = .;
    *(.ARM.exidx*);
    __exidx_end = .;
    . = ALIGN(8);
  } > ROM

  .preinit_array (READONLY) :
  {
    . = ALIGN(8);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*));
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(8);
  } > ROM

  .init_array (READONLY) :
  {
    . = ALIGN(8);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)));
    KEEP (*(.init_array*));
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(8);
  } > ROM

  .fini_array (READONLY) :
  {
    . = ALIGN(8);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)));
    KEEP (*(.fini_array*));
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(8);
  } > ROM

  .copy.table (READONLY) :
  {
    . = ALIGN(8);
    __copy_table_start__ = .;
    LONG(LOADADDR(.data));
    LONG (ADDR(.data));
    LONG (SIZEOF(.data) / 4);
    __copy_table_end__ = .;
  } > ROM

  .zero.table (READONLY) :
  {
    . = ALIGN(8);
    __zero_table_start__ = .;
    LONG (ADDR(.bss));
    LONG (SIZEOF(.bss) / 4);
    __zero_table_end__ = .;
  } > ROM

  /* First in RAM in every image: the app/bootloader hand-over block
   * (boot_shared.h). Never zeroed, never loaded. Added for the control app. */
  .boot_shared (NOLOAD) :
  {
    KEEP(*(.boot_shared))
    . = ORIGIN(RAM) + 16;
  } > RAM

  .data :
  {
    . = ALIGN(8);
    _sidata = LOADADDR(.data);
    __data_start__ = .;
    _sdata = .;
    *(.data);
    *(.data*);
    . = ALIGN(8);
    _edata = .;
  } > RAM AT> ROM

  .bss :
  {
    . = ALIGN(8);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss);
    *(.bss*);
    *(COMMON);
    . = ALIGN(8);
    _ebss = .;
    __bss_end__ = _ebss;
  } > RAM

  /* Survives a reset (not zeroed, not loaded): boot counter for the bootId
   * (device_id.c), later the fault_guard breadcrumb. Garbage after power-on;
   * every user validates it with a magic word. Added for the control app. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(8);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(8);
  } > RAM

  .heap (NOLOAD) :
  {
    . = ALIGN(8);
    __end__ = .;
    PROVIDE (end = .);
    _heap_start = .;
    . += HEAP_SIZE;
    . = ALIGN(8);
    _heap_end = .;
    __HeapLimit = .;
  } > RAM

  .stack (NOLOAD) :
  {
    . = ALIGN(8);
    __StackLimit = .;
    . += STACK_SIZE;
    . = ALIGN(8);
    __StackTop = .;
    _estack = .;
    __stack = .;
  } > RAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 :
  {
    *(.ARM.attributes)
  }
}
