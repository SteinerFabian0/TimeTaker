/* App behind the bootloader: 4 pages (32 KB) of ctrl_boot first, settings
 * excluded. image_header.h. */
MEMORY
{
  ROM (rx)  : org = 0x08008000, len = 0x34000
  RAM (xrw) : org = 0x20000000, len = 0x10000
}
