# Everything the lane firmware adds on top of the CubeMX2-generated build.
#
# Kept out of the root CMakeLists.txt on purpose: CubeMX2 overwrites that file
# on every regeneration (docs/RELEASING.md), while this one is ours. After a
# regeneration the root file only needs its one include line back.
#
# The ST-generated cmake/*.cmake files are all gated on CMAKE_BUILD_TYPE ==
# "debug_GCC_STM32C542CCU6", so EVERY preset reuses that build type and the
# LANE_RELEASE option applies the delta on top.
option(LANE_RELEASE "Production build: -O2 + RELEASE define" OFF)

file(GLOB_RECURSE LANE_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/*.c)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${LANE_APP_SOURCES})

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
  user_modifiable/_debug_GCC_STM32C542CCU6
)

# Strict warnings, as errors, on app/ only: ST's HAL is not ours to fix.
# #warning stays a warning so the RELEASE switch checks in lane_master.c do
# not block a bench build of a release image. No -Wundef: the force-included
# RTE_Components.h trips it and cannot be made a system header.
set_source_files_properties(${LANE_APP_SOURCES} PROPERTIES COMPILE_OPTIONS
  "-Wextra;-Wshadow;-Wstrict-prototypes;-Wdouble-promotion;-Wformat=2;-Wcast-align;-Wpointer-arith;-Wnull-dereference;-Werror;-Wno-error=cpp"
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PUBLIC
  $<$<BOOL:${LANE_RELEASE}>:RELEASE>
)

# Last -O flag on the command line wins; these come after the generated
# flags.cmake (-O0), so they are the effective optimization level.
target_compile_options(${CMAKE_PROJECT_NAME} PUBLIC
  $<IF:$<BOOL:${LANE_RELEASE}>,-O2,-Og>
)

# The DFP passes the linker script via -T but does not register it as a link
# dependency, so editing it would not relink. Track it explicitly.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/user_modifiable/Device/STM32C542CCU6/stm32c542xc_flash.ld
)

include(cmake/version.cmake)
