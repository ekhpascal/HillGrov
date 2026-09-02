idf_component_register(SRCS "bootloader_start.c"
                    REQUIRES bootloader bootloader_support)

# IDF 6.0.1 no longer ships a fixed bootloader.ld / bootloader.rom.ld path under
# subproject/main/ld/<target>/ (only a bootloader.ld.in template) -- use the
# BOOTLOADER_LINKER_SCRIPT build property instead, as the shipped
# examples/custom_bootloader/bootloader_override example does.
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
