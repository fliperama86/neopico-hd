#include "settings.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include <string.h>

#include "audio_source.h"
#include "pico.h"

// Last 4 KB sector of flash, reserved for settings. The firmware image is tiny
// (~75 KB) vs 16 MB flash, so the top sector never collides with code/data.
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_MAGIC 0x4E504753u // "NPGS"
#define SETTINGS_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size; // sizeof(neopico_settings_t); guards layout changes
    neopico_settings_t payload;
    uint32_t crc; // CRC-32 of payload only
} settings_record_t;

_Static_assert(sizeof(settings_record_t) <= FLASH_PAGE_SIZE, "settings record must fit one flash page");

static uint32_t settings_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static void settings_defaults(neopico_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->resolution = 0; // 480p
}

bool settings_load(neopico_settings_t *out)
{
    const settings_record_t *rec = (const settings_record_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (rec->magic == SETTINGS_MAGIC && rec->version == SETTINGS_VERSION &&
        rec->payload_size == (uint16_t)sizeof(neopico_settings_t) &&
        rec->crc == settings_crc32(&rec->payload, sizeof(neopico_settings_t))) {
        *out = rec->payload;
        return true;
    }
    settings_defaults(out);
    return false;
}

// Runs from RAM: the wrapper must not be fetched from flash while XIP is
// suspended for the erase/program. (Belt-and-suspenders; copy_to_ram builds
// already place everything in RAM.)
void __no_inline_not_in_flash_func(settings_save)(const neopico_settings_t *s)
{
    // Build the full page image first (CRC computed here, before XIP is off).
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xFF, sizeof(page));
    settings_record_t *rec = (settings_record_t *)page;
    rec->magic = SETTINGS_MAGIC;
    rec->version = SETTINGS_VERSION;
    rec->payload_size = (uint16_t)sizeof(neopico_settings_t);
    rec->payload = *s;
    rec->crc = settings_crc32(&rec->payload, sizeof(neopico_settings_t));

    const uint32_t saved = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(saved);
}

void __no_inline_not_in_flash_func(settings_factory_reset)(void)
{
    neopico_settings_t defaults;
    settings_defaults(&defaults);
    defaults.audio_source = (uint8_t)AUDIO_SOURCE_MV1C_DIGITAL;
    defaults.audio_source_valid = NEOPICO_SETTINGS_AUDIO_SOURCE_VALID;
    settings_save(&defaults);
}
