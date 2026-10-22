/*
 * Thumb-mode implementations of string/memory primitives.
 *
 * These replace the ARM-mode stubs from libc_nano.a that would otherwise
 * cause "dangerous relocation" link errors on arm-none-eabi-gcc 14.x when
 * mixing Thumb object files with the non-multilib libc archive.
 *
 * Keep implementations simple — GCC inlines calls to well-known C built-ins
 * at -O2, so these are mainly used by TinyUSB and FreeRTOS internals.
 */

#include <stddef.h>
#include <stdint.h>

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && (unsigned char)*a == (unsigned char)*b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (unsigned char)*a == (unsigned char)*b) {
        a++; b++; n--;
    }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

/* Simple decimal-only strtol — covers the interval argument parsing. */
long strtol(const char *s, char **endptr, int base) {
    long result = 0;
    int  sign   = 1;
    (void)base;                /* only base 10 used in this project */
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    const char *start = s;
    while (*s >= '0' && *s <= '9')
        result = result * 10 + (*s++ - '0');
    if (endptr)
        *endptr = (*s != '\0' && s == start) ? (char *)s : (char *)s;
    return sign * result;
}
