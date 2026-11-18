/* Standalone app (bring-up default): no bootloader, linked at the flash base.
 * The settings pages at the end of bank 2 are excluded. image_header.h. */
MEMORY
{
  ROM (rx)  : org = 0x08000000, len = 0x3C000
  RAM (xrw) : org = 0x20000000, len = 0x10000
}
