# Master-only: the embedded web UI is served by the master's http_srv, and
# hillgrow_embed_web() forces the blobs into the link (WHOLE_ARCHIVE), so
# letting it run for the zone/rescue builds would push 64 KB of assets into
# images that have no HTTP UI at all. Same guard shape as hg_mcfg's
# mcfg_store.c: IDF configures components after the top-level project() call,
# so CMAKE_PROJECT_NAME already names the app being built.
set(WEB_ASSETS_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND WEB_ASSETS_SRCS "web_assets.c")
endif()

idf_component_register(SRCS ${WEB_ASSETS_SRCS} INCLUDE_DIRS ".")

if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    # Must be called from the component's OWN directory scope -- the scope that
    # owns the library target the generated .gz.S sources are attached to
    # (Task 1 finding; calling it from a parent scope silently substitutes a
    # no-op stub for the producing command).
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/hillgrow_web.cmake)
    hillgrow_embed_web(web_assets)
endif()
