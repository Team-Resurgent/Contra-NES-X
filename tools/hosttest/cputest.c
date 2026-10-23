/* cputest.c -- the port's 6502 (src/core/cpu6502.c) against a 6502 test
 * program: Klaus Dormann's 6502_functional_test (github.com/Klaus2m5/
 * 6502_65C02_functional_tests, bin_files/; not included), or any 64 KB
 * memory image that ends by looping on one address.
 *
 *     cputest 6502_functional_test.bin [start] [pass] [stop]
 *
 * Runs from `start` (hex, default 0400) until the program counter stays on
 * one address (the test's traps are `jmp *` / `bne *`). Passing is ending at
 * `pass` (default 3469) -- or reaching `stop` (default 336D, the test's
 * decimal-mode section: the 2A03 has no decimal mode, so the binary tests
 * are what this CPU must pass). Exit status 0 on a pass. */
#include <stdio.h>
#include <stdlib.h>

#include "../../src/core/cpu6502.h"

static uint8_t sMem[65536];

static uint8_t rd(void *ctx, uint16_t a)
{
    (void)ctx;
    return sMem[a];
}

static void wr(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    sMem[a] = v;
}

int main(int argc, char **argv)
{
    Cpu6502 c = { 0 };
    unsigned start = 0x0400, pass = 0x3469, stop = 0x336D;
    unsigned long long n = 0, cycles = 0;
    FILE *f;

    if (argc < 2)
    {
        fprintf(stderr, "usage: cputest image.bin [start] [pass] [stop]\n");
        return 2;
    }
    if (argc > 2)
        start = (unsigned)strtoul(argv[2], NULL, 16);
    if (argc > 3)
        pass = (unsigned)strtoul(argv[3], NULL, 16);
    if (argc > 4)
        stop = (unsigned)strtoul(argv[4], NULL, 16);
    f = fopen(argv[1], "rb");
    if (!f || fread(sMem, 1, sizeof(sMem), f) != sizeof(sMem))
    {
        fprintf(stderr, "cputest: %s is not a 64 KB image\n", argv[1]);
        return 2;
    }
    fclose(f);
    c.read = rd;
    c.write = wr;
    cpu_reset(&c);
    c.pc = (uint16_t)start;
    for (;;)
    {
        uint16_t before = c.pc;

        cycles += (unsigned long long)cpu_step(&c);
        n++;
        if (c.jammed)
        {
            printf("cputest: jammed at %04X (opcode %02X) after %llu instructions\n", c.pc, sMem[c.pc], n);
            return 1;
        }
        if (c.pc == stop && stop != pass)
        {
            printf("cputest: reached %04X after %llu instructions, %llu cycles: the binary tests pass\n",
                   stop, n, cycles);
            return 0;
        }
        if (c.pc == before)
        {
            printf("cputest: %s at %04X after %llu instructions, %llu cycles (A=%02X X=%02X Y=%02X P=%02X S=%02X; test %02X)\n",
                   c.pc == pass ? "PASSED" : "FAILED", c.pc, n, cycles, c.a, c.x, c.y, c.p, c.s, sMem[0x200]);
            return c.pc == pass ? 0 : 1;
        }
    }
}
