# components/ is in EXTRA_COMPONENT_DIRS for master, zone AND rescue (see
# each app's CMakeLists.txt) -- IDF discovers and registers every component
# under it for all three app builds, whether or not that app's main actually
# REQUIRES it (wifi_mgr/CMakeLists.txt documents the same fact for the same
# reason). esp_hosted, by contrast, is a MANAGED component that only exists
# at all when the target is esp32p4 (master/main/idf_component.yml gates
# espressif/esp_hosted on `target == esp32p4`; zone and rescue never declare
# it and never build for esp32p4 either). An unconditional
# REQUIRES/PRIV_REQUIRES esp_hosted here would therefore break the zone
# build, the rescue build AND an esp32 (non-P4) master build -- not because
# any of them link cp_ota's P4 code, but because IDF evaluates every
# discovered component's REQUIRES to build the requirements graph before it
# knows what actually ends up linked (this project has already lost time to
# exactly this shape of bug once: a master-only component's registry
# dependency broke the zone build until the zone declared it too).
#
# CONFIG_IDF_TARGET_ESP32P4 turned out NOT to be a safe REQUIRES/PRIV_REQUIRES
# gate here, despite matching components/soc/CMakeLists.txt's own
# `if(CONFIG_IDF_TARGET_ESP32P4)` pattern -- verified against a real P4 build
# of this component, not assumed: soc's own use of that pattern only ever
# feeds its SRCS/INCLUDE_DIRS lists, never REQUIRES, and a `list(APPEND ...)`
# into PRIV_REQUIRES gated on it before idf_component_register() here silently
# produced an EMPTY requirements list even on a real esp32p4 build -- the -I
# flags for esp_hosted's headers never made it into cp_ota.c's actual compile
# command, and the build failed with IDF's own diagnostic ("cp_ota.c includes
# eh_host_cp_ota.h, provided by espressif__esp_hosted... not in the
# requirements list of cp_ota"). REQUIRES is resolved during an early
# "trial"/requirements-expansion pass over every discovered component's
# CMakeLists.txt (same pass wifi_mgr's and hg_mcfg's CMakeLists.txt comments
# warn CMAKE_PROJECT_NAME isn't safe in either) and CONFIG_IDF_TARGET_ESP32P4
# is apparently not reliably resolved as a plain CMake variable that early.
#
# idf_component_optional_requires(), by contrast, runs AFTER
# idf_component_register() -- i.e. in the later, fully-configured pass -- and
# checks the already-finalized BUILD_COMPONENTS property (component.cmake) for
# whether the named component is actually part of THIS build, no-op if not.
# That is exactly the property we need: espressif__esp_hosted is in
# BUILD_COMPONENTS only for an esp32p4 master build (master/main/idf_component.yml
# gates it on `target == esp32p4`), so this correctly attaches the dependency
# there and is a silent no-op on zone, rescue and an esp32 (non-P4) master --
# the same idiom components/wifi_mgr/CMakeLists.txt uses for espressif__mdns,
# which isn't present in every app build either. Also matches the plain
# ("esp_hosted" vs "espressif__esp_hosted") naming lesson from the failed
# attempt above: BUILD_COMPONENTS lists it as espressif__esp_hosted.
idf_component_register(
    SRCS "cp_ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition
    PRIV_REQUIRES hg_blob
)

idf_component_optional_requires(PRIVATE espressif__esp_hosted)
