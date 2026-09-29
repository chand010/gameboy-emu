#ifndef GB_APU_H
#define GB_APU_H

#include "gb.h"

u8  apu_read_register(gb_t *gb, u8 off);
void apu_write_register(gb_t *gb, u8 off, u8 value);
void apu_step(gb_t *gb, int cycles);
void apu_reset(gb_t *gb);

#endif /* GB_APU_H */
