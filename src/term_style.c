#include "term_style.h"
#include <math.h>
#include <string.h>

/* The first 16 slots are user-configurable ANSI colours. Never select them. */
uint32_t term_index_rgb(unsigned index) {
    static const unsigned levels[] = {0, 95, 135, 175, 215, 255};
    if (index >= 232 && index <= 255) {
        unsigned c = 8 + 10 * (index - 232);
        return (c << 16) | (c << 8) | c;
    }
    if (index < 16 || index > 231) return 0;
    unsigned n = index - 16;
    return (levels[n / 36] << 16) | (levels[(n / 6) % 6] << 8) | levels[n % 6];
}
static double linear(unsigned component) {
    double s = (double)component / 255.0;
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}
static double luminance(uint32_t rgb) {
    return 0.2126 * linear((rgb >> 16) & 255) +
           0.7152 * linear((rgb >> 8) & 255) + 0.0722 * linear(rgb & 255);
}
double term_contrast(uint32_t a, uint32_t b) {
    double x = luminance(a), y = luminance(b);
    return x >= y ? (x + 0.05) / (y + 0.05) : (y + 0.05) / (x + 0.05);
}
static uint32_t blend(uint32_t from, uint32_t to, unsigned amount) {
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned a = (from >> shift) & 255, b = (to >> shift) & 255;
        result |= ((a * (255 - amount) + b * amount + 127) / 255) << shift;
    }
    return result;
}
static uint32_t contrast_text(uint32_t fg, uint32_t bg) {
    if (term_contrast(fg, bg) >= 7.0) return fg;
    uint32_t target = term_contrast(0, bg) > term_contrast(0xffffff, bg) ? 0 : 0xffffff;
    for (unsigned t = 1; t < 256; ++t) {
        uint32_t candidate = blend(fg, target, t);
        if (term_contrast(candidate, bg) >= 7.0) return candidate;
    }
    return target;
}
static uint32_t contrast_background(uint32_t bg) {
    /* Mid-grey cannot provide 7:1 even with black/white foreground. Adjust it
       before selecting text colours, including arbitrary Python themes. */
    uint32_t target = luminance(bg) < 0.18 ? 0 : 0xffffff;
    uint32_t text = target ? 0 : 0xffffff;
    for (unsigned t = 0; t < 256; ++t) {
        uint32_t candidate = blend(bg, target, t);
        if (term_contrast(text, candidate) >= 7.5) return candidate;
    }
    return target;
}
static unsigned distance(uint32_t a, uint32_t b) {
    unsigned result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        int d = (int)((a >> shift) & 255) - (int)((b >> shift) & 255);
        result += (unsigned)(d * d);
    }
    return result;
}
static unsigned quantize(uint32_t rgb, uint32_t bg, bool foreground) {
    unsigned best = 16, cost = ~0u;
    for (unsigned i = 16; i <= 255; ++i) {
        uint32_t candidate = term_index_rgb(i);
        if (foreground && term_contrast(candidate, bg) < 7.0) continue;
        /* A quantized background must also admit a 7:1 foreground. */
        if (!foreground && term_contrast(0, candidate) < 7.0 &&
            term_contrast(0xffffff, candidate) < 7.0) continue;
        unsigned d = distance(rgb, candidate);
        if (d < cost) { best = i; cost = d; }
    }
    return best;
}
bool term_colors_parse(const char *name, TermColors *mode) {
    if (!name || !strcmp(name, "auto")) *mode = TERM_COLORS_AUTO;
    else if (!strcmp(name, "truecolor")) *mode = TERM_COLORS_RGB;
    else if (!strcmp(name, "256")) *mode = TERM_COLORS_256;
    else if (!strcmp(name, "mono")) *mode = TERM_COLORS_MONO;
    else return false;
    return true;
}
TermColors term_colors_resolve(TermColors requested, const char *term, const char *colorterm) {
    if (requested != TERM_COLORS_AUTO) return requested;
    (void)colorterm;
    if (!term) term = "";
    if (!strcmp(term, "dumb") || !strcmp(term, "linux")) return TERM_COLORS_MONO;
    /* A remote TERM or COLORTERM value is not a capability negotiation.
       In particular, pre-0.71 PuTTY parses unsupported semicolon RGB operands
       as separate SGR attributes (21 = underline, 31 = red). Never send both
       colour grammars as a fallback. AUTO chooses indexed colour only, even
       when COLORTERM claims truecolor; RGB is an explicit user opt-in. */
    static const char *const names[] = {
        "xterm", "putty", "screen", "tmux", "rxvt", "foot", "kitty",
        "wezterm", "alacritty", "st-", "konsole", "256color", "direct"
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i)
        if (strstr(term, names[i])) return TERM_COLORS_256;
    return TERM_COLORS_MONO;
}
void term_palette_update(TermPalette *palette, const uint32_t source[10]) {
    if (palette->initialized && !memcmp(palette->source, source, sizeof palette->source)) return;
    memcpy(palette->source, source, sizeof palette->source);
    uint32_t bg = contrast_background(source[1]);
    for (int i = 0; i < TERM_FACE_COUNT; ++i) {
        TermFace *f = &palette->face[i];
        f->bg = i == 8 || i == 9 ? contrast_background(source[i]) : bg;
        f->fg = contrast_text(source[i == 1 || i >= 8 ? 0 : i], f->bg);
        /* Reverse video preserves selection/mode-line boundaries if a client
           rejects colour changes. Swap the colour operands below so accepted
           colour pairs still have the intended foreground and background. */
        f->reverse = i == 8 || i == 9;
        if (i == TERM_CURSOR_FACE) {
            f->fg = 0x101018; f->bg = 0xfff4b8;
            f->reverse = true;
        }
        f->bg256 = quantize(f->bg, 0, false);
        f->fg256 = quantize(f->fg, term_index_rgb(f->bg256), true);
    }
    palette->initialized = true;
}
void term_face_write(FILE *out, const TermPalette *palette, TermColors mode, int face) {
    if (face < 0 || face >= TERM_FACE_COUNT) face = 0;
    const TermFace *f = &palette->face[face];
    /* A caller that has not resolved AUTO must also get the safe behaviour. */
    if (mode == TERM_COLORS_AUTO) mode = TERM_COLORS_256;
    if (mode == TERM_COLORS_MONO) {
        /* Ordinary text stays plain. Do not underline syntax, blank cells,
           selections or the cursor; do not request bold/dim/blink either. */
        fputs(f->reverse ? "\033[0;7m" : "\033[0m", out);
        return;
    }
    /* Reset the complete SGR state before every face. In particular, remove
       underline/bold/blink left behind by an earlier program or old release. */
    fputs(f->reverse ? "\033[0;7m" : "\033[0m", out);
    unsigned fg256 = f->reverse ? f->bg256 : f->fg256;
    unsigned bg256 = f->reverse ? f->fg256 : f->bg256;
    uint32_t fg = f->reverse ? f->bg : f->fg, bg = f->reverse ? f->fg : f->bg;
    if (mode == TERM_COLORS_RGB) {
        fprintf(out, "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um",
                (fg >> 16) & 255, (fg >> 8) & 255, fg & 255,
                (bg >> 16) & 255, (bg >> 8) & 255, bg & 255);
    } else {
        /* Separate foreground/background sequences, using only slots 16-255.
           No RGB suffix: unknown RGB is not guaranteed to be ignored. */
        fprintf(out, "\033[38;5;%um\033[48;5;%um", fg256, bg256);
    }
}
