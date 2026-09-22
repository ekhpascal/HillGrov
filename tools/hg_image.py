"""Read the chip an ESP app image was built for, straight from its own
header, so tools/flash_app.py and tools/flash_all.py can refuse to write an
image built for one chip into a board of a different chip -- regardless of
whether the offsets happen to be right.

Why this module exists: --target on both tools picks a set of flash
OFFSETS (flash_app.py's APP_OFFSET, flash_all.py's FLASH_LAYOUT). Nothing
upstream of esptool ever checked that the file actually sitting at that
offset was built for that chip -- only that some file existed there. Three
independent reviews found the same failure class (correct offset, wrong
architecture), reachable via a stale or missing --build-dir; two instances
were closed by routing every path through --build-dir, but that only helps
if the caller gets --build-dir right. This module closes the class instead
of one more instance of it: it reads the chip the image itself declares and
refuses on mismatch, so a wrong --build-dir (or a wrong file dropped in the
right directory) gets caught even when nobody remembers why it matters.

Every ESP app image (bootloader.bin, and every hillgrow_*.bin OTA app,
rescue included) starts with esp_image_header_t (components/
bootloader_support/include/esp_app_format.h, IDF 6.0.1 --
C:\\esp\\v6.0.1\\esp-idf\\components\\bootloader_support\\include\\
esp_app_format.h): __attribute__((packed)), 24 bytes total (confirmed by
that header's own ESP_STATIC_ASSERT). magic is a uint8 at offset 0 (must be
0xE9, ESP_IMAGE_HEADER_MAGIC) and chip_id (esp_chip_id_t, itself
__attribute__((packed)) to force exactly 2 bytes) is a little-endian uint16
at offset 12. Field layout ahead of chip_id, all packed with no padding:
magic(1) + segment_count(1) + spi_mode(1) + {spi_speed:4,spi_size:4}(1) +
entry_addr(4) + wp_pin(1) + spi_pin_drv[3](3) = 12 bytes, then chip_id.
Verified against the real file: the first 16 bytes of
rescue/build/hillgrow_rescue.bin are
e9 06 02 20 44 15 08 40 ee 00 00 00 00 00 00 00 -- chip_id (bytes 12-13)
reads 0x0000 (ESP_CHIP_ID_ESP32), matching that rescue is only ever built
for the ESP32 master today.

NOT covered here, deliberately: tools/flash_app.py's --app zonefw and --app
cpfw. Both stage a PAYLOAD for a chip other than --target into a MASTER data
partition by design -- a zone app image is always ESP32 regardless of the
master's --target, and a coprocessor image is always ESP32-C6. Running this
module's require_target_chip() against --target on either would break two
working paths; see flash_app.py for where those branches deliberately skip
it. (Those payloads end up wrapped behind a 16-byte HGFW header once
staged, inner esp_image_header_t at +16 -- but nothing here reads that
offset, on purpose: see flash_app.py for why.)
"""
import struct
import sys

IMAGE_MAGIC = 0xE9            # ESP_IMAGE_HEADER_MAGIC, esp_app_format.h
CHIP_ID_OFFSET = 12           # esp_image_header_t.chip_id, little-endian uint16
HEADER_LEN = 24               # sizeof(esp_image_header_t), packed (static-asserted in the header)

# esp_chip_id_t (esp_app_format.h) -- every value it currently defines,
# spelled the same way esptool's own --chip flag spells the chip (lowercase,
# no hyphen), so this dict doubles as target-name <-> chip_id in both
# directions. Taken from the enum itself, not memorized: chip_id is assigned
# in registration order, not chip-generation order (note C6 0x0D < H2 0x10 <
# P4 0x12 < C61 0x14 < C5 0x17).
CHIP_ID_NAME = {
    0x0000: "esp32",
    0x0002: "esp32s2",
    0x0005: "esp32c3",
    0x0009: "esp32s3",
    0x000C: "esp32c2",
    0x000D: "esp32c6",
    0x0010: "esp32h2",
    0x0012: "esp32p4",
    0x0014: "esp32c61",
    0x0017: "esp32c5",
    0x0019: "esp32h21",
    0x001C: "esp32h4",
}
TARGET_CHIP_ID = {name: chip_id for chip_id, name in CHIP_ID_NAME.items()}


def read_chip_id(path, what):
    """Return the little-endian chip_id at offset 12 of the ESP app image
    header at the START of `path` (offset 0 -- every caller in this repo
    checks a real app image, never a wrapped payload; see module docstring).
    Exits (sys.exit) with a message naming `what` and `path` if the file is
    too short or doesn't start with the ESP image magic byte -- such a file
    isn't an app image at all, and reading its chip_id would be reading
    garbage, producing a confusing "wrong chip" refusal instead of the real
    problem."""
    with open(path, "rb") as f:
        header = f.read(HEADER_LEN)
    if len(header) < HEADER_LEN:
        sys.exit(f"error: {what} is too short to be an ESP app image "
                  f"({len(header)} bytes, need {HEADER_LEN}): {path}")
    if header[0] != IMAGE_MAGIC:
        sys.exit(f"error: {what} does not start with the ESP app image magic "
                  f"byte ({header[0]:#04x}, expected {IMAGE_MAGIC:#04x}) -- "
                  f"this isn't an app image: {path}")
    return struct.unpack_from("<H", header, CHIP_ID_OFFSET)[0]


def require_target_chip(path, target, what):
    """Refuse (sys.exit) if the ESP app image at `path` was not built for
    `target` (a --target string, e.g. "esp32" or "esp32p4") -- the specific
    guard this module exists for. The message names the file, the chip it
    was actually built for, the --target requested, and what to do about
    it, because a warning on a destructive flash path is a warning nobody
    reads."""
    got_id = read_chip_id(path, what)
    want_id = TARGET_CHIP_ID[target]
    if got_id != want_id:
        got_name = CHIP_ID_NAME.get(got_id, f"unknown chip (chip_id {got_id:#06x})")
        sys.exit(
            f"error: {what} was built for {got_name}, but --target {target} "
            f"means this is going to a {target} board: {path}\n"
            f"  refusing to flash an image built for a different chip than "
            f"--target -- rebuild {what} for {target} (and check --build-dir "
            f"points at that build), or pass --target {got_name} if that's "
            f"what you actually meant to flash"
        )
