/* App behind the bootloader: 3 pages (24 KB) of ctrl_boot first, settings
 * excluded. image_header.h. */
MEMORY
{
  ROM (rx)  : org = 0x08006000, len = 0x36000
  RAM (xrw) : org = 0x20000000, len = 0x10000
}
