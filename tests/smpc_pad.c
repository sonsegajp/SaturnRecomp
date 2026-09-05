#include "saturn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static saturn S;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    uint8_t report[7];
    unsigned length;

    memset(&S, 0, sizeof S);
    S.pad1_x = S.pad1_y = 0x80;
    S.pad1_lo = 0x04; /* A pressed, active high internally */
    length = smpc_pad_report(&S, report);
    check(length == 3 && report[0] == 0x02,
          "standard pad keeps the two-byte 0x02 report");
    check(report[1] == 0xFB && report[2] == 0xFF,
          "standard pad buttons are active low");

    S.pad1_analog = 1;
    S.pad1_lo = 0x80; /* Right */
    S.pad1_hi = 0x88; /* R and L */
    S.pad1_x = 0x00;
    S.pad1_y = 0xFF;
    S.pad1_l = 0x12;
    S.pad1_r = 0xE7;
    length = smpc_pad_report(&S, report);
    check(length == 7 && report[0] == 0x16,
          "analog mode reports 3D pad type 1 with six data bytes");
    check(report[1] == 0x7F && report[2] == 0x77,
          "3D pad digital bytes and fixed low bits match hardware");
    check(report[3] == 0x00 && report[4] == 0xFF &&
          report[5] == 0xE7 && report[6] == 0x12,
          "3D pad reports X Y R L in INTBACK order");

    /* Direct TH/TR/TL access starts with type and length nibbles. */
    S.smpc_reg[0x79 & 0x7F] = 0x60;
    S.smpc_reg[0x75 & 0x7F] = 0x40;
    check(smpc_pdr_read(&S, 0) == 0xFF,
          "TH high resets the 3D pad serial report");
    S.smpc_reg[0x75 & 0x7F] = 0x00;
    check(smpc_pdr_read(&S, 0) == 0x01,
          "first 3D pad handshake nibble is type 1");
    S.smpc_reg[0x75 & 0x7F] = 0x20;
    check(smpc_pdr_read(&S, 0) == 0x15,
          "second 3D pad handshake nibble declares six-byte report");
    S.smpc_reg[0x75 & 0x7F] = 0x00;
    check(smpc_pdr_read(&S, 0) == 0x07,
          "handshake proceeds into active-low button nibbles");

    puts("SMPC pad: all tests passed");
    return 0;
}
