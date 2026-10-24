#pragma once
#include <stdint.h>

/* Each function appends to p (no terminating NUL) and returns the new end. */

char   *fmt_str(char *p, const char *s);
/* v right-aligned in a field of width chars */
char   *fmt_int(char *p, int32_t v, int width);
/* "0x1E" */
char   *fmt_hex8(char *p, uint8_t v);
/* up to 2 decimals, trailing zeros dropped: "50", "7.5", "0.75" */
char   *fmt_dec2(char *p, float v);
/* 3 decimals: "1.012", "-0.987" */
char   *fmt_fixed3(char *p, float v);

int32_t round_i32(float v);
