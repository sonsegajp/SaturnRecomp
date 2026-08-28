/* Render a raw `saturn` diagnostic snapshot without running either CPU.
 *
 * Snapshots may contain user-supplied BIOS/disc data and therefore belong only
 * in ignored local output directories.  This tool is deliberately generic and
 * contains no game data. */
#include "saturn.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    saturn *s;
    uint32_t *frame;
    FILE *f;
    int w, h;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: render_state <state.bin> <frame.png> [layer-mask]\n");
        return 2;
    }

    s = (saturn *)malloc(sizeof(*s));
    if (!s) {
        fprintf(stderr, "error: cannot allocate %zu-byte Saturn state\n", sizeof(*s));
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open %s\n", argv[1]);
        free(s);
        return 1;
    }
    if (fread(s, 1, sizeof(*s), f) != sizeof(*s)) {
        fprintf(stderr, "error: %s is not a complete %zu-byte state\n",
                argv[1], sizeof(*s));
        fclose(f);
        free(s);
        return 1;
    }
    fclose(f);

    vdp2_display_size(s, &w, &h);
    if (w < 1 || w > 704) w = 320;
    if (h < 1 || h > 512) h = 224;
    frame = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(*frame));
    if (!frame) {
        free(s);
        return 1;
    }
    s->layer_lock = 1;
    s->layer_mask = argc == 4 ? (unsigned)strtoul(argv[3], NULL, 0) : 0x3Fu;
    vdp2_render(s, frame, w, h, 1);
    if (png_write(argv[2], frame, w, h) != 0) {
        fprintf(stderr, "error: cannot write %s\n", argv[2]);
        free(frame);
        free(s);
        return 1;
    }
    printf("rendered %s (%dx%d) from %s\n", argv[2], w, h, argv[1]);
    free(frame);
    free(s);
    return 0;
}
