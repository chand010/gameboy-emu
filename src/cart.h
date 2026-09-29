#ifndef GB_CART_H
#define GB_CART_H

#include "gb.h"

int  cart_load(gb_t *gb, const char *rom_path);
void cart_save(gb_t *gb);
void cart_destroy(gb_t *gb);
void cart_reset(gb_t *gb);

u8   cart_read(gb_t *gb, u16 addr);
void cart_write(gb_t *gb, u16 addr, u8 value);

#endif /* GB_CART_H */
