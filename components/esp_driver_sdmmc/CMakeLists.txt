# HILLGROW OVERRIDE of IDF 6.0.1's esp_driver_sdmmc (see README.md in this directory). components/ is an
# EXTRA_COMPONENT_DIRS entry for master, zone and rescue, and IDF lets a project-extra component replace an IDF
# component of the same name, so this copy is the one every app links. It differs from IDF 6.0.1 only in sd_host_isr().
# Pinned: any other IDF version stops the configure, so an upgrade cannot silently keep (or lose) the patch.
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "6.0.1")
    message(FATAL_ERROR "components/esp_driver_sdmmc overrides IDF 6.0.1's driver, but this is IDF "
                        "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}: re-check the ISR fix "
                        "(components/esp_driver_sdmmc/README.md) against the new IDF, re-copy, and update this pin.")
endif()

idf_build_get_property(target IDF_TARGET)

set(srcs)

set(public_include "include" "legacy/include")

# SDMMC related source files
if(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
    list(APPEND srcs "legacy/src/sdmmc_transaction.c"
                     "legacy/src/sdmmc_host.c"
                     "src/sd_host_sdmmc.c"
                     "src/sd_trans_sdmmc.c")
endif()

if(${target} STREQUAL "linux")
    set(requires "")
    set(priv_requires esp_timer)
else()
    set(requires esp_driver_sd_intf sdmmc)
    set(priv_requires esp_timer esp_pm esp_mm esp_driver_gpio esp_hal_sd)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${public_include}
                       REQUIRES "${requires}"
                       PRIV_REQUIRES "${priv_requires}"
                      )
