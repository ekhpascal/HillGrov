# Pure: no IDF headers in time_core.c, so no component REQUIRES either --
# this is exactly the "compiles the same .c file the host tests link" pure
# component, same shape as app_version/hg_blob.
idf_component_register(SRCS "time_core.c" INCLUDE_DIRS ".")
