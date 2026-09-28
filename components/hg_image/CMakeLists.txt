# Pure image identification (recovery design 4.2 / 5.7): no IDF dependency, so every app can link it and the host suite
# compiles it straight from source (tests/host/test_hg_image.c).
idf_component_register(SRCS "hg_image.c" INCLUDE_DIRS ".")
