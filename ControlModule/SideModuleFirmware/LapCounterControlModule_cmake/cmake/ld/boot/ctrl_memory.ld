/* The bootloader (ctrl_boot): the first 3 pages. image_header.h. */
MEMORY
{
  ROM (rx)  : org = 0x08000000, len = 0x6000
  RAM (xrw) : org = 0x20000000, len = 0x10000
}
