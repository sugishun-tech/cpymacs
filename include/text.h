#ifndef CPYMACS_TEXT_H
#define CPYMACS_TEXT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef struct Piece Piece;
typedef struct {
    Piece *root;
    char *original, *added;
    size_t original_len, added_len, added_cap;
    uint32_t random;
} Text;
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void text_init(Text *t, const char *s, size_t n);
void text_free(Text *t);
size_t text_size(const Text *t);
size_t text_lines(const Text *t);
size_t text_chars(const Text *t);
bool text_insert(Text *t, size_t at, const char *s, size_t n);
bool text_delete(Text *t, size_t at, size_t n);
char *text_slice(const Text *t, size_t at, size_t n);
void text_copy(const Text *t, size_t at, size_t n, char *out);
unsigned char text_byte(const Text *t, size_t at);
size_t text_next(const Text *t, size_t at);
size_t text_prev(const Text *t, size_t at);
size_t text_line_start(const Text *t, size_t line);
size_t text_line_of(const Text *t, size_t at);
size_t text_char_to_byte(const Text *t, size_t ch);
size_t text_byte_to_char(const Text *t, size_t at);
bool text_write(const Text *t, FILE *f, bool crlf);
bool utf8_valid(const char *s, size_t n);
uint32_t utf8_decode(const char *s, size_t n, size_t *used);
bool text_boundary(const Text *t, size_t at);
int text_check(const Text *t);
#endif
