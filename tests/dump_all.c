/* dump_all — emit our decoder's text for every 16-bit opcode, in the same
 * format as tests/oracle/capstone_sh2_dump.py, for a 0-wrong diff. */
#include "../external/sh2-recomp-core/common/sh2_isa.h"
#include <stdio.h>

#define ADDR 0x06004000u

int main(int argc, char **argv)
{
    char buf[64];
    FILE *out = stdout;

    if (argc > 1) {
        out = fopen(argv[1], "wb");   /* binary: no CRLF translation */
        if (!out) { perror(argv[1]); return 1; }
    }
    for (unsigned op = 0; op < 0x10000; op++) {
        if (sh2_format((uint16_t)op, ADDR, buf))
            fprintf(out, "%04X\t%s\n", op, buf);
        else
            fprintf(out, "%04X\t.invalid\n", op);
    }
    if (out != stdout) fclose(out);
    return 0;
}
