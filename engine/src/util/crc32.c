#include "crc32.h"

/* CRC-32/ISO-HDLC (same polynomial used in zlib/PNG/Ethernet). */

static uint32_t s_table[256];
static int      s_table_init = 0;

static void table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_table[i] = c;
    }
    s_table_init = 1;
}

uint32_t crc32_compute(const void *data, size_t len)
{
    if (!s_table_init)
        table_init();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = s_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
