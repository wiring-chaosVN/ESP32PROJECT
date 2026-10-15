# Custom second-stage bootloader hooks. These internal ESP-IDF headers expose the
# ROM-backed flash API and are intentionally pinned to the selected IDF version.
set(IDF_BOOTLOADER_SUPPORT_DIR "$ENV{IDF_PATH}/components/bootloader_support")
idf_component_register(
    SRCS "hooks.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${IDF_BOOTLOADER_SUPPORT_DIR}/include"
        "${IDF_BOOTLOADER_SUPPORT_DIR}/include_bootloader"
)
