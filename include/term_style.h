#ifndef CPYMACS_TERM_STYLE_H
#define CPYMACS_TERM_STYLE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Frontend-local policy: never changes a shared backend or the GUI theme. */
typedef enum {
    TERM_COLORS_AUTO, TERM_COLORS_RGB, TERM_COLORS_256, TERM_COLORS_MONO
} TermColors;
enum { TERM_FACE_COUNT = 11, TERM_CURSOR_FACE = 10 };
typedef struct {
    uint32_t fg, bg;
    unsigned fg256, bg256;
    bool reverse;
} TermFace;
typedef struct {
    uint32_t source[10];
    TermFace face[TERM_FACE_COUNT];
    bool initialized;
} TermPalette;
bool term_colors_parse(const char *name, TermColors *mode);
TermColors term_colors_resolve(TermColors requested, const char *term, const char *colorterm);
uint32_t term_index_rgb(unsigned index);
double term_contrast(uint32_t a, uint32_t b);
void term_palette_update(TermPalette *palette, const uint32_t source[10]);
void term_face_write(FILE *out, const TermPalette *palette, TermColors mode, int face);
#endif
