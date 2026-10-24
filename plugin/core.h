#ifndef VITA_CONTROL_CORE_H
#define VITA_CONTROL_CORE_H
#include <stddef.h>
#include <stdint.h>

#define VC_PORT 5000
#define VC_MAX_WIDTH 960u
#define VC_MAX_HEIGHT 544u
#define VC_MAX_RGB (VC_MAX_WIDTH * VC_MAX_HEIGHT * 3u)
#define VC_MAX_REQUEST 128u
#define VC_MAX_DURATION 5000u
#define VC_BUTTONS 0x0000F3F9u
#define VC_AUTH 1u
#define VC_INFO 2u
#define VC_SCREENSHOT 3u
#define VC_INPUT 4u
#define VC_RELEASE 5u
#define VC_OK 0u
#define VC_BAD_REQUEST 1u
#define VC_UNAUTHORIZED 2u
#define VC_NO_FRAME 3u
#define VC_UNSUPPORTED_FRAME 4u

uint32_t vc_read32(const uint8_t *p);
void vc_write32(uint8_t *p, uint32_t n);
int vc_token_equal(const uint8_t *a, size_t an, const char *b, size_t bn);
/* Convert A8B8G8R8 memory (RGBA bytes), stripping pitch and alpha. */
int vc_copy_rgb(uint8_t *dst, const uint8_t *src, uint32_t width,
                uint32_t height, uint32_t pitch, uint32_t format);
int vc_valid_input(const uint8_t *payload, size_t length);
uint32_t vc_map_buttons(uint32_t buttons, int extended);
#endif
