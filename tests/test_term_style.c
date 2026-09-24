#define _GNU_SOURCE
#include "term_style.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static const uint32_t cyberpunk[] = {
    0xe4e4ef,0x181818,0x878787,0xffaf00,0xd7ff87,
    0xff5faf,0x5fd7ff,0xffaf5f,0x4e3a65,0x303030
};
static uint32_t random32(void) {
    static uint32_t state = 0x134892;
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
static double check_palette(const uint32_t source[10]) {
    TermPalette palette = {0};
    term_palette_update(&palette, source);
    CHECK(palette.initialized);
    CHECK(!memcmp(palette.source, source, sizeof palette.source));
    double minimum = 21.0;
    for (int i=0; i<TERM_FACE_COUNT; ++i) {
        TermFace *f = &palette.face[i];
        double rgb = term_contrast(f->fg, f->bg);
        double indexed = term_contrast(term_index_rgb(f->fg256), term_index_rgb(f->bg256));
        CHECK(rgb >= 7.0); CHECK(indexed >= 7.0);
        CHECK(f->fg256 >= 16 && f->fg256 <= 255);
        CHECK(f->bg256 >= 16 && f->bg256 <= 255);
        if (rgb < minimum) minimum = rgb;
        if (indexed < minimum) minimum = indexed;
        for (int m=TERM_COLORS_AUTO; m<=TERM_COLORS_MONO; ++m) {
            char *text=NULL; size_t length=0; FILE *stream=open_memstream(&text,&length);
            CHECK(stream); term_face_write(stream,&palette,(TermColors)m,i); CHECK(!fclose(stream));
            CHECK(!strstr(text,"\033]")); /* No OSC writes, including global palette/cursor. */
            CHECK(!strstr(text,"\033[1m")); CHECK(!strstr(text,"\033[2m"));
            CHECK((strstr(text,"38;2;") != NULL) == (m==TERM_COLORS_RGB));
            CHECK((strstr(text,"38;5;") != NULL) == (m==TERM_COLORS_AUTO || m==TERM_COLORS_256));
            CHECK(!strstr(text,"\033[4m")); CHECK(!strstr(text,"\033[0;4m"));
            CHECK(!strstr(text,"\033[21m")); CHECK(!strstr(text,"\033[0;21m"));
            CHECK(!strstr(text,"\033[0;7;4m"));
            CHECK(!strncmp(text,"\033[0m",4) || !strncmp(text,"\033[0;7m",6));
            free(text);
        }
    }
    TermPalette before=palette;
    term_palette_update(&palette,source);
    CHECK(!memcmp(&before,&palette,sizeof palette));
    return minimum;
}
int main(void) {
    TermColors mode;
    CHECK(term_colors_parse(NULL,&mode) && mode==TERM_COLORS_AUTO);
    CHECK(term_colors_parse("auto",&mode) && mode==TERM_COLORS_AUTO);
    CHECK(term_colors_parse("truecolor",&mode) && mode==TERM_COLORS_RGB);
    CHECK(term_colors_parse("256",&mode) && mode==TERM_COLORS_256);
    CHECK(term_colors_parse("mono",&mode) && mode==TERM_COLORS_MONO);
    CHECK(!term_colors_parse("typo",&mode)); CHECK(!term_colors_parse("",&mode));
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"xterm",NULL)==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"putty",NULL)==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"screen-256color",NULL)==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"tmux-256color",NULL)==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"vt100",NULL)==TERM_COLORS_MONO);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"dumb","truecolor")==TERM_COLORS_MONO);
    CHECK(term_colors_resolve(TERM_COLORS_256,"vt100",NULL)==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,NULL,NULL)==TERM_COLORS_MONO);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"xterm","truecolor")==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"putty","24bit")==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"xterm-direct",NULL)==TERM_COLORS_256);
    CHECK(term_colors_resolve(TERM_COLORS_RGB,"putty",NULL)==TERM_COLORS_RGB);
    CHECK(term_colors_resolve(TERM_COLORS_AUTO,"vt100","truecolor")==TERM_COLORS_MONO);
    const uint32_t defaults[] = {0xd9e1eb,0x15191f,0x748292,0xffca85,0xb6dc86,
                                0xe699e6,0x86c9ed,0xe89d8b,0x365070,0x222a35};
    check_palette(defaults);
    double minimum=check_palette(cyberpunk);
    for (unsigned n=0;n<256;++n) {
        uint32_t colors[10];
        for (int i=0;i<10;++i) colors[i]=n<32?0x777777:random32()&0xffffff;
        check_palette(colors);
    }
    printf("PASS: 258 palettes; 7:1 RGB and indexed contrast; no ANSI slots 0-15; cached palette; exclusive colour modes; no underline\n");
    printf("Cyberpunk minimum foreground/background contrast (RGB and 256-colour): %.4f:1\n",minimum);
    return 0;
}
