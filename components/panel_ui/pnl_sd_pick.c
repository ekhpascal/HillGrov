#include <string.h>
#include "pnl_sd_pick.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int pnl_sd_is_bin_name(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *e = name + n - 4;
    return e[0] == '.' && lower(e[1]) == 'b' && lower(e[2]) == 'i' && lower(e[3]) == 'n';
}

pnl_fw_class_t pnl_sd_classify(const pnl_sd_file_t *f, uint16_t self_chip, char version[33]) {
    if (version) version[0] = '\0';
    if (!f || f->size == 0) return PNL_FW_UNKNOWN;
    hg_image_id_t id;
    if (hg_image_parse(f->hdr, f->hdr_len, &id) != 0) return PNL_FW_UNKNOWN;
    pnl_fw_class_t c = PNL_FW_UNKNOWN;
    if (strcmp(id.project, HG_PROJ_MASTER) == 0)                                   c = id.chip_id == self_chip ? PNL_FW_MASTER : PNL_FW_WRONG_CHIP;
    else if (strcmp(id.project, HG_PROJ_ZONE) == 0)                                c = id.chip_id == HG_CHIP_ESP32 ? PNL_FW_ZONE : PNL_FW_WRONG_CHIP;
    else if (strcmp(id.project, HG_PROJ_CP) == 0 && id.chip_id == HG_CHIP_ESP32C6) c = PNL_FW_RADIO;
    if (c != PNL_FW_UNKNOWN && version) memcpy(version, id.version, sizeof id.version);
    return c;
}

static int rank(pnl_fw_class_t c) {
    switch (c) {
    case PNL_FW_MASTER:     return 0;
    case PNL_FW_ZONE:       return 1;
    case PNL_FW_RADIO:      return 2;
    case PNL_FW_WRONG_CHIP: return 3;
    default:                return 4;
    }
}

void pnl_sd_sort(pnl_sd_file_t *v, pnl_fw_class_t *cls, int n) {
    for (int i = 1; i < n; i++) {           /* insertion sort: n <= 16 and it is stable */
        pnl_sd_file_t key = v[i];
        pnl_fw_class_t kc = cls[i];
        int j = i - 1;
        while (j >= 0 && (rank(cls[j]) > rank(kc) || (rank(cls[j]) == rank(kc) && strcmp(v[j].name, key.name) > 0))) {
            v[j + 1] = v[j];
            cls[j + 1] = cls[j];
            j--;
        }
        v[j + 1] = key;
        cls[j + 1] = kc;
    }
}
