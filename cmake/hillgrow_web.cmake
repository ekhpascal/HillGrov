# Gzips web/ into ${CMAKE_BINARY_DIR}/web and attaches the .gz files as EMBED_FILES of COMPONENT.
# Fails the build when tools/web_pack.py exits non-zero (assets over the 64 KB cap).
function(hillgrow_embed_web COMPONENT)
    set(REPO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..)
    set(WEB_OUT ${CMAKE_BINARY_DIR}/web)
    set(GZ ${WEB_OUT}/index.html.gz ${WEB_OUT}/app.js.gz ${WEB_OUT}/app.css.gz)
    add_custom_command(OUTPUT ${GZ}
        COMMAND ${PYTHON} ${REPO}/tools/web_pack.py --src ${REPO}/web --out ${WEB_OUT} --cap 65536
        DEPENDS ${REPO}/web/index.html ${REPO}/web/app.js ${REPO}/web/app.css ${REPO}/tools/web_pack.py
        COMMENT "HillGrow web assets (gzip + size cap)")
    add_custom_target(hillgrow_web_assets DEPENDS ${GZ})
    idf_component_get_property(lib ${COMPONENT} COMPONENT_LIB)
    add_dependencies(${lib} hillgrow_web_assets)
    foreach(f ${GZ})
        target_add_binary_data(${lib} ${f} BINARY)
    endforeach()
    # WHOLE_ARCHIVE: the linker only pulls .o members out of a static component
    # archive when something already in the link references a symbol in them.
    # Nothing calls the _binary_*_gz_start/_end symbols until the HTTP handler
    # (a later task) is wired up, so without this the embedded blobs would be
    # silently dropped and the symbols would never reach the .map/.elf.
    idf_component_set_property(${COMPONENT} WHOLE_ARCHIVE TRUE)
endfunction()
