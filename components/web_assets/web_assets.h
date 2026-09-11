#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The three gzipped web-UI files, embedded in the master image.
 *
 * cmake/hillgrow_web.cmake's hillgrow_embed_web() gzips web/index.html,
 * web/app.js and web/app.css into build/web as .gz files and attaches them to THIS
 * component as binary data (symbols _binary_index_html_gz_start/_end etc),
 * with WHOLE_ARCHIVE so the linker keeps blobs nothing else references. The
 * component exists purely to scope that WHOLE_ARCHIVE to the blobs: it used
 * to sit on `main`, which as the app grows would defeat dead-code stripping
 * for the whole application (Task 1 review).
 *
 * name is the ORIGINAL file name ("index.html", "app.js", "app.css"); the
 * returned bytes are the GZIP stream, so a caller serving them over HTTP must
 * set Content-Encoding: gzip. ctype gets the uncompressed media type. NULL
 * (leaving len and ctype untouched) for an unknown name. */
const uint8_t *web_asset(const char *name, size_t *len, const char **ctype);

#ifdef __cplusplus
}
#endif
