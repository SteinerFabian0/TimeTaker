# Everything the control firmware adds on top of the CubeMX2-generated build.
#
# Kept out of the root CMakeLists.txt on purpose: CubeMX2 overwrites that file
# on every regeneration (docs/RELEASING.md), while this one is ours. After a
# regeneration the root file only needs its one include line back.
#
# The ST-generated cmake/*.cmake files are all gated on CMAKE_BUILD_TYPE ==
# "debug_GCC_main", so EVERY preset reuses that build type and the CTRL_RELEASE
# option applies the delta on top. Same scheme as the lane module.
option(CTRL_RELEASE "Production build: -O2 + RELEASE define" OFF)

file(GLOB_RECURSE CTRL_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/*.c)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${CTRL_APP_SOURCES})

target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC
  app/config
  app/coordinators
  app/modules
)

# Vendor headers as SYSTEM, so the strict app warnings below stop at our own
# code. GCC drops a -I that is also given as -isystem, so this overrides the
# generated flags.cmake without editing it.
target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PUBLIC
  arch/cmsis/CMSIS/Core/Include
  generated/hal
  stm32c5xx_dfp/Include
  stm32c5xx_drivers/hal
  stm32c5xx_drivers/ll
  stm32c5xx_drivers/timebases
  user_modifiable/_debug_GCC_main
)

# Strict warnings, as errors, on app/ only: ST's HAL and TinyUSB are not ours
# to fix. #warning stays a warning so the RELEASE switch checks in
# ctrl_master.c do not block a bench build of a release image. No -Wundef: the
# force-included RTE_Components.h trips it and cannot be made a system header.
set_source_files_properties(${CTRL_APP_SOURCES} PROPERTIES COMPILE_OPTIONS
  "-Wextra;-Wshadow;-Wstrict-prototypes;-Wdouble-promotion;-Wformat=2;-Wcast-align;-Wpointer-arith;-Wnull-dereference;-Werror;-Wno-error=cpp"
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC
  $<$<BOOL:${CTRL_RELEASE}>:RELEASE>
)

# Last -O flag on the command line wins; these come after the generated
# flags.cmake (-O0), so they are the effective optimization level.
target_compile_options(${CMAKE_PROJECT_NAME} PUBLIC
  $<IF:$<BOOL:${CTRL_RELEASE}>,-O2,-Og>
)

# The DFP passes the linker script via -T but does not register it as a link
# dependency, so editing it would not relink. Track it explicitly.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/user_modifiable/Device/STM32C542CCU6/stm32c542xc_flash.ld
)


# --- USB: TinyUSB owns the USB_DRD_FS peripheral ----------------------------
#
# CubeMX2 always generates mx_usb_drd_fs.c with a HAL PCD init AND the
# USB_DRD_FS_IRQHandler, and there is no switch to drop the handler. TinyUSB
# brings its own handler, so the generated file is kept out of the build
# here, and usb_cdc.c supplies the one function mx_system_init() calls from it
# (mx_usb_drd_fs_device_init: clocks + NVIC priority, no PCD). This survives
# every regeneration. If CubeMX ever renames that function, the link fails
# loudly. See usb_cdc.h.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/generated/hal/mx_usb_drd_fs.c
  PROPERTIES HEADER_FILE_ONLY ON
)

set(TINYUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/tinyusb/src)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  ${TINYUSB_DIR}/tusb.c
  ${TINYUSB_DIR}/common/tusb_fifo.c
  ${TINYUSB_DIR}/device/usbd.c
  ${TINYUSB_DIR}/class/cdc/cdc_device.c
  ${TINYUSB_DIR}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
  ${TINYUSB_DIR}/portable/st/stm32_fsdev/fsdev_common.c
)

# tusb_config.h lives in app/config, which is already on the include path.
target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PUBLIC ${TINYUSB_DIR})

include(cmake/version.cmake)


# Plain binary next to the .elf, for STM32CubeProgrammer over SWD (v1 flashes
# every image that way; no bootloader, no field update).
find_program(CTRL_OBJCOPY arm-none-eabi-objcopy REQUIRED)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
  COMMAND ${CTRL_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
  VERBATIM)
