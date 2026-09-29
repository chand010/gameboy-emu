/*
 * gbemu - a Game Boy emulator in C
 *
 * timer.c - the DIV divider and the TIMA timer with its interrupt.
 */

#include <string.h>

#include "gb.h"

/* TIMA clock divisor (in T-cycles) for each TAC frequency selector. */
static const int tima_divisors[4] = {
    1024,   /* 4096 Hz  */
    16,     /* 262144 Hz */
    64,     /* 65536 Hz */
    256,    /* 16384 Hz */
};

void timer_step(gb_t *gb, int cycles)
{
    timer_state_t *t = &gb->timer;

    /* DIV increments at 16384 Hz = every 256 T-cycles. */
    t->div_accum += cycles;
    while (t->div_accum >= 256) {
        t->div_accum -= 256;
        t->div = (u16)(t->div + 1);
    }

    /* A pending overflow reloads TIMA from TMA and requests an interrupt.
     * We resolve it one step later so that TIMA reads 0x00 briefly, which
     * matches the real hardware closely enough. */
    if (t->tima_overflow) {
        t->tima = t->tma;
        gb_request_interrupt(gb, 2);   /* timer interrupt */
        t->tima_overflow = false;
    }

    if (!(t->tac & 0x04))
        return;                        /* timer disabled */

    t->tima_accum += cycles;
    int div = tima_divisors[t->tac & 0x03];

    while (t->tima_accum >= div) {
        t->tima_accum -= div;
        t->tima = (u8)(t->tima + 1);
        if (t->tima == 0x00)
            t->tima_overflow = true;
    }
}
