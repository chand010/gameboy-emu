#ifndef GB_JOYPAD_H
#define GB_JOYPAD_H

#include "gb.h"

void joypad_reset(gb_t *gb);
u8   joypad_read(gb_t *gb);
void joypad_write(gb_t *gb, u8 value);

#endif /* GB_JOYPAD_H */
