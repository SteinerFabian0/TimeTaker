# Same as the nRF52 DK: power/clock/bprot and acl/flash-controller share
# addresses on the nRF52805.
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")
