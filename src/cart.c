/*
 * gbemu - a Game Boy emulator in C
 *
 * cart.c - cartridge loading and memory bank controller (MBC) emulation.
 *
 * ROM only, MBC1, MBC2, MBC3 and MBC5 are supported. MBC3 includes a very
 * small real time clock that is derived from the host clock, which is enough
 * for games that just want the date to be plausible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gb.h"
#include "cart.h"

/* Number of 16KB ROM banks indicated by the cartridge header. */
static const u16 rom_bank_count[0x60] = {
    [0x00] = 2,    [0x01] = 4,    [0x02] = 8,    [0x03] = 16,
    [0x04] = 32,   [0x05] = 64,   [0x06] = 128,  [0x07] = 256,
    [0x08] = 512,  [0x52] = 72,   [0x53] = 80,   [0x54] = 96,
};

/* Number of 8KB RAM banks indicated by the cartridge header. */
static const u8 ram_bank_count[0x06] = {
    [0x00] = 0,    [0x01] = 0,    [0x02] = 1,    [0x03] = 4,
    [0x04] = 16,   [0x05] = 8,
};

static const char *mbc_name(u8 type)
{
    switch (type) {
        case 0x00:                 return "ROM only";
        case 0x01: case 0x02: case 0x03: return "MBC1";
        case 0x05: case 0x06:           return "MBC2";
        case 0x0F: case 0x10: case 0x11:
        case 0x12: case 0x13:           return "MBC3";
        case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: return "MBC5";
        default:                        return "unsupported";
    }
}

static char *ram_path_for_rom(const char *rom_path)
{
    size_t len = strlen(rom_path);
    char *path = malloc(len + 5);
    if (!path) return NULL;

    strcpy(path, rom_path);

    /* swap the trailing ".gb"/".gbc" for ".sav" */
    if (len >= 4 &&
        (strcmp(path + len - 4, ".gbc") == 0 || strcmp(path + len - 3, ".gb") == 0)) {
        char *dot = strrchr(path, '.');
        if (dot) strcpy(dot, ".sav");
        else strcat(path, ".sav");
    } else {
        strcat(path, ".sav");
    }

    return path;
}

void cart_reset(gb_t *gb)
{
    cart_t *c = &gb->cart;

    c->ram_enabled = false;
    c->rom_bank = 1;
    c->ram_bank = 0;
    c->mbc1_mode = false;
    c->rtc_select = 0;
    c->rtc_latched = false;
    c->rtc_latch_time = 0;
    memset(c->rtc_regs, 0, sizeof(c->rtc_regs));
}

int cart_load(gb_t *gb, const char *rom_path)
{
    cart_t *c = &gb->cart;

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "cart: could not open '%s'\n", rom_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0x150) {
        fprintf(stderr, "cart: '%s' is too small to be a Game Boy ROM\n", rom_path);
        fclose(f);
        return -1;
    }

    c->rom = malloc((size_t)size);
    if (!c->rom) {
        fclose(f);
        return -1;
    }

    if (fread(c->rom, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cart: short read on '%s'\n", rom_path);
        fclose(f);
        return -1;
    }
    fclose(f);

    c->rom_size = (size_t)size;
    c->rom_banks = (size_t)size / 0x4000;
    if (c->rom_banks < 1) c->rom_banks = 1;

    u8 type = c->rom[0x147];
    u8 rom_code = c->rom[0x148];
    u8 ram_code = c->rom[0x149];

    switch (type) {
        case 0x00: c->mbc = MBC_NONE; break;
        case 0x01: case 0x02: case 0x03: c->mbc = MBC_MBC1; break;
        case 0x05: case 0x06:           c->mbc = MBC_MBC2; break;
        case 0x0F: case 0x10: case 0x11:
        case 0x12: case 0x13:           c->mbc = MBC_MBC3; break;
        case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: c->mbc = MBC_MBC5; break;
        default:
            fprintf(stderr, "cart: unsupported cartridge type 0x%02X (%s)\n",
                    type, mbc_name(type));
            return -1;
    }

    c->has_ram    = (ram_code >= 0x02);
    c->has_battery = (type == 0x03 || type == 0x06 || type == 0x09 ||
                      type == 0x0D || type == 0x0F || type == 0x10 ||
                      type == 0x13 || type == 0x1B || type == 0x1E);

    size_t header_ram_banks = ram_code <= 0x05 ? ram_bank_count[ram_code] : 0;
    if (c->mbc == MBC_MBC2) header_ram_banks = 1;   /* 512 nibbles */

    c->ram_banks = header_ram_banks;
    if (c->ram_banks == 0 && c->has_ram) c->ram_banks = 1;

    if (c->mbc == MBC_MBC2) {
        c->ram_size = 512;
    } else {
        c->ram_size = (size_t)c->ram_banks * 0x2000;
    }

    if (c->ram_size > 0) {
        c->ram = calloc(1, c->ram_size);
        if (!c->ram) return -1;
    }

    /* keep the advertised bank count within the actual file */
    if (rom_code < 0x60 && rom_bank_count[rom_code] < c->rom_banks)
        c->rom_banks = rom_bank_count[rom_code];

    /* try to load an existing save file */
    c->ram_path = ram_path_for_rom(rom_path);
    if (c->ram_size > 0 && c->has_battery && c->ram_path) {
        FILE *s = fopen(c->ram_path, "rb");
        if (s) {
            size_t r = fread(c->ram, 1, c->ram_size, s);
            (void)r;
            fclose(s);
        }
    }

    char title[17];
    memset(title, 0, sizeof(title));
    memcpy(title, &c->rom[0x134], c->rom[0x143] <= 15 ? c->rom[0x143] : 16);

    printf("cart: %-16s | %-12s | %d banks | RAM %s%s\n",
           title, mbc_name(type), (int)c->rom_banks,
           c->has_ram ? "yes" : "no",
           c->has_battery ? " + battery" : "");

    cart_reset(gb);
    return 0;
}

void cart_save(gb_t *gb)
{
    cart_t *c = &gb->cart;

    if (c->ram_size == 0 || !c->has_battery || !c->ram_path)
        return;

    FILE *f = fopen(c->ram_path, "wb");
    if (!f) {
        fprintf(stderr, "cart: could not write save '%s'\n", c->ram_path);
        return;
    }
    fwrite(c->ram, 1, c->ram_size, f);
    fclose(f);
}

void cart_destroy(gb_t *gb)
{
    cart_t *c = &gb->cart;

    cart_save(gb);

    free(c->rom);    c->rom = NULL;
    free(c->ram);    c->ram = NULL;
    free(c->ram_path); c->ram_path = NULL;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

static u8 cart_read_rom(cart_t *c, u16 addr)
{
    /* 0x0000-0x3FFF: fixed bank 0
     * 0x4000-0x7FFF: switchable bank (bank 1 lives at file offset 0x4000) */
    size_t offset;
    if (addr < 0x4000) {
        offset = addr;
    } else {
        u16 bank = c->rom_bank;
        if (bank >= c->rom_banks)
            bank = (u16)(c->rom_banks - 1);
        offset = (size_t)bank * 0x4000 + (addr - 0x4000);
    }

    if (offset >= c->rom_size)
        return 0xFF;
    return c->rom[offset];
}

static u8 mbc3_rtc_read(cart_t *c)
{
    /* latch the RTC from the host clock when asked */
    u64 now_ms = (u64)time(NULL) * 1000;

    if (c->rtc_select <= 0x0C) {
        if (!c->rtc_latched) {
            /* decode host time into BCD RTC registers */
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            c->rtc_regs[0] = (u8)((tm->tm_sec / 10 << 4) | (tm->tm_sec % 10));
            c->rtc_regs[1] = (u8)((tm->tm_min / 10 << 4) | (tm->tm_min % 10));
            c->rtc_regs[2] = (u8)((tm->tm_hour / 10 << 4) | (tm->tm_hour % 10));
            c->rtc_regs[3] = 0x01; /* day counter low nibble */
            c->rtc_regs[4] = 0x00;
            c->rtc_latched = true;
            c->rtc_latch_time = now_ms;
        }
        (void)now_ms;
    }

    if (c->rtc_select >= 0x08 && c->rtc_select <= 0x0C)
        return c->rtc_regs[c->rtc_select - 0x08];

    return 0xFF;
}

u8 cart_read(gb_t *gb, u16 addr)
{
    cart_t *c = &gb->cart;

    if (addr < 0x8000)
        return cart_read_rom(c, addr);

    /* external RAM at 0xA000-0xBFFF */
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!c->ram_enabled)
            return 0xFF;

        if (c->mbc == MBC_MBC3 && c->rtc_select >= 0x08)
            return mbc3_rtc_read(c);

        if (c->ram_size == 0)
            return 0xFF;

        if (c->mbc == MBC_MBC2) {
            /* 512x4bit built in RAM */
            size_t offset = (addr - 0xA000) & 0x1FF;
            u8 nibble = (addr & 1) ? (c->ram[offset] >> 4) : (c->ram[offset] & 0x0F);
            return nibble | 0xF0;
        }

        size_t offset = (size_t)(c->ram_bank % c->ram_banks) * 0x2000 + (addr - 0xA000);
        if (offset >= c->ram_size)
            return 0xFF;
        return c->ram[offset];
    }

    return 0xFF;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

void cart_write(gb_t *gb, u16 addr, u8 value)
{
    cart_t *c = &gb->cart;

    switch (c->mbc) {
        case MBC_NONE:
            return;

        case MBC_MBC1: {
            if (addr < 0x2000) {
                c->ram_enabled = ((value & 0x0F) == 0x0A);
            } else if (addr < 0x4000) {
                u8 bank = value & 0x1F;
                if (bank == 0) bank = 1;
                c->rom_bank = (c->rom_bank & 0x60) | bank;
            } else if (addr < 0x6000) {
                if (c->mbc1_mode) {
                    c->ram_bank = value & 0x03;
                } else {
                    c->rom_bank = (c->rom_bank & 0x1F) | ((value & 0x03) << 5);
                }
            } else {
                c->mbc1_mode = (value & 0x01) != 0;
            }
            if ((c->rom_bank & 0x1F) == 0)
                c->rom_bank |= 1;
            return;
        }

        case MBC_MBC2: {
            if (addr < 0x4000) {
                if (addr & 0x0100) {
                    u8 bank = value & 0x0F;
                    if (bank == 0) bank = 1;
                    c->rom_bank = bank;
                } else {
                    c->ram_enabled = ((value & 0x0F) == 0x0A);
                }
            }
            return;
        }

        case MBC_MBC3: {
            if (addr < 0x2000) {
                c->ram_enabled = ((value & 0x0F) == 0x0A);
            } else if (addr < 0x4000) {
                u8 bank = value & 0x7F;
                if (bank == 0) bank = 1;
                c->rom_bank = bank;
            } else if (addr < 0x6000) {
                c->rtc_select = value;
                c->ram_bank = (value & 0x03);
            } else {
                /* RTC latch on 0 -> 1 transition */
                if (value == 0x01 && c->rtc_latch_value == 0x00)
                    c->rtc_latched = false;
                c->rtc_latch_value = value;
            }
            return;
        }

        case MBC_MBC5: {
            if (addr < 0x2000) {
                c->ram_enabled = ((value & 0x0F) == 0x0A);
            } else if (addr < 0x3000) {
                c->rom_bank = (c->rom_bank & 0x100) | value;
            } else if (addr < 0x4000) {
                c->rom_bank = (c->rom_bank & 0xFF) | ((value & 0x01) << 8);
            } else if (addr < 0x6000) {
                c->ram_bank = value & 0x0F;
            }
            if (c->rom_bank == 0) c->rom_bank = 1;
            return;
        }
    }
}
