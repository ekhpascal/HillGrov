"""Build a valid ESP-IDF "otadata" (OTA select) partition image that boots
straight into ota_0, for flashing in place of the stock (all-0xFF)
ota_data_initial.bin.

Why: with a blank otadata partition (both 32-byte select entries all-0xFF)
*and* a factory partition present, bootloader_utility_get_selected_boot_partition()
(components/bootloader_support/src/bootloader_utility.c:407-414 in IDF 6.0.1)
selects FACTORY, not ota_0 -- and nothing ever repairs otadata afterwards,
because that repair path (set_actual_ota_seq) only runs when there is *no*
factory partition. Today (zone/master have only a stub/no rescue image at
the factory offset) this just falls through with an "invalid image" log and
ota_0 boots anyway -- but once Task 16 puts a real rescue image at 0x30000,
every freshly flashed board would boot into rescue permanently.

The fix: write otadata ourselves, selecting ota_0 outright, instead of
flashing the blank ota_data_initial.bin.

Struct layout (components/bootloader_support/include/esp_flash_partitions.h):

    typedef struct {
        uint32_t ota_seq;          // offset 0
        uint8_t  seq_label[20];    // offset 4
        uint32_t ota_state;        // offset 24
        uint32_t crc;              // offset 28 -- CRC32 of ota_seq field only
    } esp_ota_select_entry_t;      // 32 bytes total

The otadata partition holds two of these, one per 4 KB flash sector
(bootloader_utility.c writes/reads each copy at `partition_offset +
FLASH_SECTOR_SIZE * i`, FLASH_SECTOR_SIZE == 0x1000 -- see
bootloader_flash_priv.h). Selection: boot_index = (ota_seq - 1) % app_count,
so ota_seq=1 selects app_count's slot 0, i.e. ota_0.

CRC: bootloader_common_ota_select_crc() in
components/bootloader_support/src/bootloader_common_loader.c:67-70 is

    uint32_t bootloader_common_ota_select_crc(const esp_ota_select_entry_t *s) {
        return esp_rom_crc32_le(UINT32_MAX, (uint8_t*)&s->ota_seq, 4);
    }

i.e. esp_rom_crc32_le(0xFFFFFFFF, &ota_seq, 4) over the raw 4-byte
little-endian ota_seq field only. esp_rom_crc32_le's own semantics are
pinned down two ways in this IDF checkout:

  1. Its source (the Linux-target drop-in reimplementation of the ROM
     function, byte-identical behaviour to the real ROM by design) --
     components/esp_rom/linux/esp_rom_crc.c:166-174:

        uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len) {
            uint32_t i;
            crc = ~crc;                                    // complement IN
            for (i = 0; i < len; i++)
                crc = crc32_le_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
            return ~crc;                                    // complement OUT
        }

     i.e. the passed-in `crc` is complemented before use as the register
     seed, and the result is complemented again before returning -- the
     standard reflected CRC-32 (poly 0xEDB88320) construction, not a bare
     unadjusted LFSR.
  2. components/esp_rom/test_apps/linux_rom_apis/main/rom_test.cpp:58 calls
     it the same way we do: esp_rom_crc32_le(0xffffffff, original, sizeof(original)).

Because of the complement-in/complement-out, esp_rom_crc32_le(0xFFFFFFFF, ...)
is bit-for-bit what Python's binascii.crc32(data, 0xFFFFFFFF) computes:
CPython's crc32(data, value) treats `value` as a "public" checksum to
continue from (internal register = value ^ 0xFFFFFFFF; final = core-result
^ 0xFFFFFFFF) -- exactly the same complement-in/complement-out shape. This
was verified empirically against both a from-scratch bit-level CRC-32
implementation and against esp_rom_crc.c's algorithm above before writing
this module (see the fix-round report for the worked example: ota_seq=1 ->
CRC 0x4743989A). It is also, unsurprisingly, exactly what IDF's own
components/app_update/otatool.py does when writing a fresh sequence number
(same repo, ~line 189-191):

    ota_seq_next = struct.pack('I', ota_seq_next)
    ota_seq_crc_next = binascii.crc32(ota_seq_next, 0xFFFFFFFF) % (1 << 32)

We replicate that call here (with an explicit '<I' pack instead of
otatool.py's native 'I' -- little-endian on any host this runs on, and
identical bytes to otatool.py's native pack on the x86/x64 hosts this
project targets).
"""
import binascii
import os
import struct

OTADATA_PARTITION_SIZE = 0x2000  # matches zone/master partitions.csv "otadata" size
OTADATA_ENTRY_SIZE = 32
ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF  # "App can boot and work without limits" -- our
                                     # apps mark VALID at runtime (app_update rollback)


def _ota_select_entry(ota_seq):
    """Pack one 32-byte esp_ota_select_entry_t selecting the given sequence number."""
    seq_bytes = struct.pack('<I', ota_seq)
    crc = binascii.crc32(seq_bytes, 0xFFFFFFFF) & 0xFFFFFFFF
    seq_label = b'\xff' * 20
    ota_state = struct.pack('<I', ESP_OTA_IMG_UNDEFINED)
    entry = seq_bytes + seq_label + ota_state + struct.pack('<I', crc)
    assert len(entry) == OTADATA_ENTRY_SIZE
    return entry


def build_otadata(ota_seq=1):
    """Build the full 8 KB otadata image.

    Entry 0 (first 4 KB sector) selects `ota_seq` (default 1 -> ota_0) and is
    valid/bootable. Entry 1 (second 4 KB sector) is left all-0xFF (blank /
    invalid) -- both mechanically, because the rest of the image past entry 0
    is 0xFF fill, and semantically, since an all-0xFF entry's ota_seq ==
    UINT32_MAX marks it invalid (bootloader_common_ota_select_invalid()).
    """
    entry0 = _ota_select_entry(ota_seq)
    image = entry0 + b'\xff' * (OTADATA_PARTITION_SIZE - OTADATA_ENTRY_SIZE)
    assert len(image) == OTADATA_PARTITION_SIZE
    return image


def write_otadata_file(build_dir, ota_seq=1):
    """Write the generated otadata image to <build_dir>/hg_otadata.bin and
    return its path."""
    path = os.path.join(build_dir, "hg_otadata.bin")
    with open(path, "wb") as f:
        f.write(build_otadata(ota_seq))
    return path
