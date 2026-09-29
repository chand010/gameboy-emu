/*
 * gbemu - a Game Boy emulator in C
 *
 * joypad.c - the P1 register and the joypad interrupt.
 */

#include "gb.h"

void joypad_reset(gb_t *gb)
{
    /* bits 4/5 of P1 select the button group; 0 means selected. The real
     * register resets to $CF, i.e. both groups selected. */
    gb->joypad.p1 = 0x00;
    gb->joypad.buttons = 0;
    gb->joypad.prev_lines = 0x0F;
}

static u8 input_lines(gb_t *gb)
{
    joypad_t *j = &gb->joypad;

    /* The four input lines are active low. */
    u8 lines = 0x0F;

    if (!(j->p1 & 0x10)) {          /* action buttons selected */
        if (j->buttons & (1 << JOYP_A))      lines &= ~0x01;
        if (j->buttons & (1 << JOYP_B))      lines &= ~0x02;
        if (j->buttons & (1 << JOYP_SELECT)) lines &= ~0x04;
        if (j->buttons & (1 << JOYP_START))  lines &= ~0x08;
    }

    if (!(j->p1 & 0x20)) {          /* direction pad selected */
        if (j->buttons & (1 << JOYP_RIGHT)) lines &= ~0x01;
        if (j->buttons & (1 << JOYP_LEFT))  lines &= ~0x02;
        if (j->buttons & (1 << JOYP_UP))    lines &= ~0x04;
        if (j->buttons & (1 << JOYP_DOWN))  lines &= ~0x08;
    }

    return lines;
}

/* Re-evaluate the input lines and request the joypad interrupt when any
 * selected line falls from high to low. */
static void joypad_update(gb_t *gb)
{
    joypad_t *j = &gb->joypad;
    u8 lines = input_lines(gb);

    if ((j->prev_lines & ~lines) & 0x0F)
        gb_request_interrupt(gb, 4);

    j->prev_lines = lines;
}

u8 joypad_read(gb_t *gb)
{
    joypad_t *j = &gb->joypad;
    u8 lines = input_lines(gb);

    /* high two bits are always set, then the select bits, then the lines */
    return 0xC0 | (j->p1 & 0x30) | lines;
}

void joypad_write(gb_t *gb, u8 value)
{
    joypad_t *j = &gb->joypad;

    /* only bits 4 and 5 are writable */
    j->p1 = value & 0x30;
    joypad_update(gb);
}

void gb_press_button(gb_t *gb, int button)
{
    if (button < 0 || button > 7)
        return;

    gb->joypad.buttons |= (u8)(1 << button);
    joypad_update(gb);
}

void gb_release_button(gb_t *gb, int button)
{
    if (button < 0 || button > 7)
        return;

    gb->joypad.buttons &= (u8)~(1 << button);
    joypad_update(gb);
}
