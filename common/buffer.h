// String Buffer
#ifndef NL_BUFFER_H
#define NL_BUFFER_H

#include "stdarg.h"

struct nl_buffer_t;
typedef struct nl_buffer_t nl_buffer_t;

struct nl_buffer_t {
    char *buf;
    int len;
    int alloc;
};

nl_buffer_t *nl_buffer_new(void);
void nl_buffer_free(nl_buffer_t *buf);

const char *nl_buffer_get(nl_buffer_t *buf);
void nl_buffer_get_free(const char *str);

void nl_buffer_format(nl_buffer_t *buf, const char *fmt, ...);

#ifdef NL_BUFFER_IMPL

nl_buffer_t *nl_buffer_new(void) {
    nl_buffer_t *buf = NL_MALLOC(sizeof(nl_buffer_t));
    buf->alloc = 16;
    buf->buf = NL_MALLOC(buf->alloc);
    buf->buf[0] = '\0';
    buf->len = 0;
    return buf;
}

void nl_buffer_free(nl_buffer_t *buf) {
    NL_FREE(buf);
}

void nl_buffer_get_free(const char *str) {
    NL_FREE((void*) str);
}

void nl_buffer_format(nl_buffer_t *buf, const char *restrict fmt, ...) {
    while (true) {
        va_list ap;
        va_start(ap, fmt);
        int written = vsnprintf(&buf->buf[buf->len], buf->alloc - buf->len, fmt, ap);
        va_end(ap);
        if (buf->len + written >= buf->alloc) {
            buf->alloc = buf->alloc + buf->alloc / 2 + written;
            buf->buf = NL_REALLOC(buf->buf, buf->alloc);
            continue;
        }
        buf->len += written;
        break;
    }
}

const char *nl_buffer_get(nl_buffer_t *buf) {
    const char *ret = buf->buf;
    NL_FREE(buf);
    return ret;
}

#endif
#endif
