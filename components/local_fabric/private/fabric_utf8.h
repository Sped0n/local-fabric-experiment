#pragma once
#include <stddef.h>
#include <string.h>

// Copy a bounded label without splitting a UTF-8 code point.
static inline void fabric_utf8_copy(char *dest, size_t n, const char *text)
{
    if (!n)
        return;
    size_t used = strlcpy(dest, text ? text : "", n);
    if (used < n)
        return;
    size_t end = n - 1;
    while (end && ((unsigned char)text[end] & 0xc0u) == 0x80u) --end;
    dest[end] = 0;
}
