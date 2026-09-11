#include <string.h>
#include "web_assets.h"

/* target_add_binary_data(... BINARY) names its symbols after the file with
 * every non-identifier character replaced by '_', so "index.html.gz" becomes
 * _binary_index_html_gz_start/_end. BINARY (not TXTFILES) means no trailing
 * NUL is appended -- the length always comes from end - start. */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[]     asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]       asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[]    asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[]      asm("_binary_app_css_gz_end");

static const struct {
    const char    *name;
    const uint8_t *start, *end;
    const char    *ctype;
} ASSETS[] = {
    { "index.html", index_html_gz_start, index_html_gz_end, "text/html"              },
    { "app.js",     app_js_gz_start,     app_js_gz_end,     "application/javascript" },
    { "app.css",    app_css_gz_start,    app_css_gz_end,    "text/css"               },
};

const uint8_t *web_asset(const char *name, size_t *len, const char **ctype) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof ASSETS / sizeof ASSETS[0]; i++) {
        if (strcmp(name, ASSETS[i].name) != 0) continue;
        if (len)   *len   = (size_t)(ASSETS[i].end - ASSETS[i].start);
        if (ctype) *ctype = ASSETS[i].ctype;
        return ASSETS[i].start;
    }
    return NULL;
}
