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

--target is NOT the right question for tools/flash_app.py's --app zonefw and
--app cpfw. Both stage a PAYLOAD for a chip other than --target into a MASTER
data partition by design -- a zone app image is always ESP32 regardless of
the master's --target, and a coprocessor image is always ESP32-C6 -- so
require_target_chip() against --target would refuse every legitimate call on
both paths. They get require_payload_chip() instead, which asks the question
that IS meaningful there: is this the chip the partition's *consumer*
expects, on every --target. Those payloads are wrapped behind a 16-byte HGFW
header once staged, so their inner esp_image_header_t sits at +16, which is
what the `offset` argument below is for. (Originally deferred: with no
ESP32-C6 image in the checkout there was no way to prove the accept path
still worked, and an unverifiable guard on a destructive path is worse than
none. The promoted coproc/ project builds one in-tree, so it is now provable
and is done.)
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


def _blame(path, offset, staged_from):
    """Which file a refusal should NAME, plus the note that says where the
    bytes were actually read from.

    `path` is always what gets read. When it is a staged scratch copy (built
    under master/build by flash_app.py's build_*_image()), `staged_from` is the
    file the operator actually passed, and that is what the message must blame:
    a refusal pointing at a scratch file in a build directory names something
    the operator never chose and cannot fix. Used by every sys.exit() in this
    module that can see a staged path, so the two cannot disagree about which
    file is the operator's."""
    if not staged_from:
        return path, ""
    return staged_from, (f"\n  (read at +{offset} inside the staged copy {path} -- "
                         f"the exact bytes esptool would have written)")


def read_chip_id(path, what, offset=0, staged_from=None):
    """Return the little-endian chip_id at +12 of the esp_image_header_t that
    starts at `offset` in `path`. offset defaults to 0 (a bare app image, as
    flashed); pass flash_app.py's HGFW_HDR_LEN to read the INNER header of a
    staged HGFW payload, whose 16-byte wrapper sits ahead of the real image.
    Exits (sys.exit) with a message naming `what` and the operator's file if
    there aren't HEADER_LEN bytes at `offset` or those bytes don't start with
    the ESP image magic byte -- such a file isn't an app image at all (or the
    wrapper isn't the size we thought), and reading its chip_id would be
    reading garbage, producing a confusing "wrong chip" refusal instead of the
    real problem.

    `staged_from` is optional and means the same thing it does in
    require_payload_chip(): the file the operator named, when `path` is the
    staged copy read on its behalf. Both exits below go through _blame(), so
    they name that file rather than the scratch copy -- they became reachable
    with a staged path when the payload check was added, and blaming `path`
    there was the exact confusion `staged_from` was introduced to remove."""
    with open(path, "rb") as f:
        f.seek(offset)
        header = f.read(HEADER_LEN)
    at = "" if offset == 0 else f" at offset {offset}"
    blamed, read_note = _blame(path, offset, staged_from)
    if len(header) < HEADER_LEN:
        sys.exit(f"error: {what} is too short to be an ESP app image{at} "
                  f"({len(header)} bytes, need {HEADER_LEN}): {blamed}{read_note}")
    if header[0] != IMAGE_MAGIC:
        sys.exit(f"error: {what} does not start with the ESP app image magic "
                  f"byte{at} ({header[0]:#04x}, expected {IMAGE_MAGIC:#04x}) -- "
                  f"this isn't an app image: {blamed}{read_note}")
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
            f"means this is going to an {target} board: {path}\n"
            f"  refusing to flash an image built for a different chip than "
            f"--target -- rebuild {what} for {target} (and check --build-dir "
            f"points at that build), or pass --target {got_name} if that's "
            f"what you actually meant to flash"
        )


def require_payload_chip(path, expect_chip, what, offset=0, staged_from=None):
    """Refuse (sys.exit) if the ESP app image at `offset` in `path` was not
    built for `expect_chip` (a chip name as spelled in CHIP_ID_NAME, e.g.
    "esp32" or "esp32c6").

    A SEPARATE guard from require_target_chip(), and deliberately so: this is
    for a payload staged into a data partition, where the expected chip is
    fixed by whatever reads that partition back and is UNRELATED to --target
    (which only picks flash offsets). Those two answers are supposed to
    differ, so they must not share one check -- a zone app payload is ESP32
    on an esp32p4 master, and a coprocessor payload is ESP32-C6 on either.
    The chip id itself comes out of CHIP_ID_NAME/TARGET_CHIP_ID above, i.e.
    out of esp_app_format.h's esp_chip_id_t enum, not spelled at the call
    site from memory.

    `path` is what gets READ (the staged copy, so the check is over the exact
    bytes about to be flashed); `staged_from` is the file the operator
    actually named, and is what the message blames -- see _blame(). Without it
    the refusal would point at a scratch file in a build directory that the
    operator never chose and cannot fix. It is passed down into read_chip_id()
    as well, so the too-short and bad-magic refusals blame the same file this
    one does.

    The message names the consumer, because the operator's next question on
    a destructive staging path is "then what was I supposed to build?"."""
    want_id = TARGET_CHIP_ID[expect_chip]
    got_id = read_chip_id(path, what, offset, staged_from)
    if got_id != want_id:
        got_name = CHIP_ID_NAME.get(got_id, f"unknown chip (chip_id {got_id:#06x})")
        blamed, read_note = _blame(path, offset, staged_from)
        sys.exit(
            f"error: {what} was built for {got_name}, but this partition's "
            f"payload must be an {expect_chip} image: {blamed}\n"
            f"  refusing to stage it -- this is NOT a --target question "
            f"(--target only picks the flash offset); the chip is fixed by "
            f"what reads the partition back on the device. Rebuild it for "
            f"{expect_chip} and pass that image instead.{read_note}"
        )
