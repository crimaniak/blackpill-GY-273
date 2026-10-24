/* Small text formatting helpers: the firmware has no printf */
#include "fmt.h"

/* Append v right-aligned in a field of width chars; returns the new end. */
char *fmt_int(char *p, int32_t v, int width) {
    char tmp[12];
    int  n = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) tmp[n++] = '-';
    for (int i = n; i < width; i++) *p++ = ' ';
    while (n) *p++ = tmp[--n];
    return p;
}

char *fmt_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

char *fmt_hex8(char *p, uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    *p++ = '0';
    *p++ = 'x';
    *p++ = hex[v >> 4];
    *p++ = hex[v & 0xF];
    return p;
}

/* Append v with up to 2 decimals: "50", "7.5", "0.75" */
char *fmt_dec2(char *p, float v) {
    int32_t c = (int32_t)(v * 100.0f + 0.5f);
    p = fmt_int(p, c / 100, 0);
    if (c % 100) {
        *p++ = '.';
        *p++ = (char)('0' + c % 100 / 10);
        if (c % 10)
            *p++ = (char)('0' + c % 10);
    }
    return p;
}

int32_t round_i32(float v) {
    return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* Append v with 3 decimals: "1.012", "-0.987" */
char *fmt_fixed3(char *p, float v) {
    int32_t m = round_i32(v * 1000.0f);
    if (m < 0) {
        *p++ = '-';
        m = -m;
    }
    p = fmt_int(p, m / 1000, 0);
    *p++ = '.';
    *p++ = (char)('0' + m / 100 % 10);
    *p++ = (char)('0' + m / 10 % 10);
    *p++ = (char)('0' + m % 10);
    return p;
}
