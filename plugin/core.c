#include "core.h"

uint32_t vc_read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
void vc_write32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
int vc_token_equal(const uint8_t *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return 0;
    uint8_t diff = 0;
    for (size_t i = 0; i < bn; ++i) diff |= a[i] ^ (uint8_t)b[i];
    return diff == 0;
}
int vc_copy_rgb(uint8_t *dst, const uint8_t *src, uint32_t width,
                uint32_t height, uint32_t pitch, uint32_t format) {
    if (!src || !width || !height || width > VC_MAX_WIDTH ||
        height > VC_MAX_HEIGHT || pitch < width || pitch > 4096 || format != 0)
        return -1;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = src + y * pitch * 4;
        for (uint32_t x = 0; x < width; ++x) {
            *dst++ = row[x * 4];
            *dst++ = row[x * 4 + 1];
            *dst++ = row[x * 4 + 2];
        }
    }
    return 0;
}
int vc_valid_input(const uint8_t *p, size_t length) {
    return length == 16 && !(vc_read32(p) & ~VC_BUTTONS) &&
           vc_read32(p + 4) >= 1 && vc_read32(p + 4) <= VC_MAX_DURATION &&
           !(vc_read32(p + 12) & ~3u);
}
uint32_t vc_map_buttons(uint32_t buttons, int extended) {
    if (!extended) return buttons;
    /* *Positive2 / *Ext2 represent handheld L/R as L1/R1. */
    return (buttons & ~0x300u) | ((buttons & 0x300u) << 2);
}
