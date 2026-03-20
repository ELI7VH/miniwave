/* miniwave — OSC binary helpers */

#ifndef MINIWAVE_OSC_H
#define MINIWAVE_OSC_H

#include <stdint.h>
#include <string.h>

#define OSC_BUF_SIZE 2048

static inline int osc_pad4(int n) { return (n + 3) & ~3; }

static inline int32_t osc_read_i32(const uint8_t *b) {
    return (int32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
}

static inline float osc_read_f32(const uint8_t *b) {
    union { uint32_t u; float f; } conv;
    conv.u = (uint32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    return conv.f;
}

static inline void osc_write_i32(uint8_t *b, int32_t v) {
    b[0] = (v >> 24) & 0xFF;
    b[1] = (v >> 16) & 0xFF;
    b[2] = (v >> 8)  & 0xFF;
    b[3] =  v        & 0xFF;
}

static inline void osc_write_f32(uint8_t *b, float v) {
    union { uint32_t u; float f; } conv;
    conv.f = v;
    osc_write_i32(b, (int32_t)conv.u);
}

static int osc_write_string(uint8_t *buf, int max, const char *str) {
    int len = (int)strlen(str) + 1;
    int padded = osc_pad4(len);
    if (padded > max) return -1;
    memcpy(buf, str, (size_t)len);
    memset(buf + len, 0, (size_t)(padded - len));
    return padded;
}

#endif
